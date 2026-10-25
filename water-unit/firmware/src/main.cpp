// ESP32 firmware sketch for the water unit.
// STATUS: written against the pinout on slide 7, NOT yet compiled or run on
// hardware. The decision logic it calls (include/decision.h) is tested natively.
// Every number marked CALIBRATE is a placeholder until you have probe data.
//
// Serial commands (115200 baud):
//   T   start one treatment cycle
//   F0  Fe/Mn strip test clear     F1  Fe/Mn flagged     FX  not tested
#include <Arduino.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <SD.h>
#include <SPI.h>
#include "decision.h"

// ---- pins (slide 7) ----
const uint8_t PIN_PH = 34, PIN_ORP = 35, PIN_TURB = 32, PIN_TDS = 33;
const uint8_t PIN_ONEWIRE = 4, PIN_FLOW = 36, PIN_LEVEL = 39;
const uint8_t PIN_SD_CS = 5;
const uint8_t PIN_RELAY_PUMP = 25, PIN_RELAY_UV = 21, PIN_RELAY_RO = 22, PIN_RELAY_IX = 13;
const uint8_t PIN_LED = 2, PIN_BUZZER = 15;
// Opto relay boards are usually active LOW. Flip if yours is not.
const uint8_t RELAY_ON = LOW, RELAY_OFF = HIGH;
const uint8_t LEVEL_OK_STATE = HIGH;  // float switch state when there is water. CALIBRATE

// ---- timing ----
const uint32_t SAMPLE_MS = 5000;            // sensor sample period
const uint8_t LOG_EVERY_N_SAMPLES = 12;     // one SD row per minute
const uint32_t TREAT_MS = 120000;           // one treatment run
const uint8_t MAX_RETRIES = 3;

// ---- calibration placeholders ----
const float PH_V4_MV = 3000.0f, PH_V7_MV = 2500.0f;     // CALIBRATE with pH 4.00 and 7.00 buffers
const float ORP_ZERO_MV = 1500.0f, ORP_GAIN = 1.0f;     // CALIBRATE with a 225 mV ORP solution
// Turbidity: probe voltage falls as water gets cloudier. CALIBRATE with known NTU samples.
const float TURB_V_MV[4] = {800.0f, 1500.0f, 2200.0f, 2800.0f};
const float TURB_NTU[4]  = {3000.0f, 500.0f, 20.0f, 0.0f};
const float PULSES_PER_LITRE = 450.0f;                  // YF-S201 nominal

OneWire oneWire(PIN_ONEWIRE);
DallasTemperature ds18(&oneWire);
wu::Controller controller;

volatile uint32_t flowPulses = 0;
void IRAM_ATTR onFlowPulse() { flowPulses++; }

enum State { MONITOR, TREATING };
State state = MONITOR;
uint32_t lastSample = 0, treatStart = 0;
uint8_t sampleCount = 0, retries = 0;
int8_t feMn = -1;
wu::Decision lastDecision = {};
bool sdOk = false;

float readMilliVolts(uint8_t pin) {
  uint32_t sum = 0;
  for (int i = 0; i < 16; i++) { sum += analogReadMilliVolts(pin); delay(2); }
  return sum / 16.0f;
}

float interp(float x, const float *xs, const float *ys, int n) {
  if (x <= xs[0]) return ys[0];
  for (int i = 1; i < n; i++)
    if (x <= xs[i]) return ys[i - 1] + (ys[i] - ys[i - 1]) * (x - xs[i - 1]) / (xs[i] - xs[i - 1]);
  return ys[n - 1];
}

// Reads all five sensors into engineering units. A disconnected probe usually
// pins the ADC at 0 or full scale, which the range check in decision.h catches.
void readSensors(float raw[wu::N_SENSORS]) {
  float vPh = readMilliVolts(PIN_PH);
  raw[wu::PH] = 4.0f + (vPh - PH_V4_MV) * (3.0f / (PH_V7_MV - PH_V4_MV));
  raw[wu::ORP] = (readMilliVolts(PIN_ORP) - ORP_ZERO_MV) * ORP_GAIN;
  raw[wu::TURB] = interp(readMilliVolts(PIN_TURB), TURB_V_MV, TURB_NTU, 4);
  // TDS: common hobby-probe polynomial, uncompensated. decision.h applies the
  // 25 C compensation. CALIBRATE against a 342 ppm NaCl standard.
  float v = readMilliVolts(PIN_TDS) / 1000.0f;
  raw[wu::TDS] = (133.42f * v * v * v - 255.86f * v * v + 857.39f * v) * 0.5f;
  ds18.requestTemperatures();
  raw[wu::TEMP] = ds18.getTempCByIndex(0);  // -127 when unplugged
}

void setRelay(uint8_t pin, bool on) { digitalWrite(pin, on ? RELAY_ON : RELAY_OFF); }

void applyStages(const wu::Decision &d, bool pumpOn) {
  setRelay(PIN_RELAY_PUMP, pumpOn);
  setRelay(PIN_RELAY_UV, pumpOn && d.s.uv);
  setRelay(PIN_RELAY_RO, pumpOn && d.s.ro);   // default (off) routes UF
  setRelay(PIN_RELAY_IX, pumpOn && d.s.ix);
}

void alertBeep(uint8_t n) {
  for (uint8_t i = 0; i < n; i++) { digitalWrite(PIN_BUZZER, HIGH); delay(120); digitalWrite(PIN_BUZZER, LOW); delay(120); }
}

void logRow(const float raw[wu::N_SENSORS], const wu::Decision &d) {
  char stages[8];
  wu::stageString(d.s, stages);
  Serial.printf("ph=%.2f orp=%.0f turb=%.1f tds25=%.0f temp=%.1f stages=%s failsafe=%d alerts=%d\n",
                d.values[wu::PH], d.values[wu::ORP], d.values[wu::TURB], d.tds25, d.values[wu::TEMP],
                stages, d.failSafe, d.alerts);
  if (!sdOk) return;
  File f = SD.open("/log.csv", FILE_APPEND);
  if (!f) return;
  f.printf("%lu,%.2f,%.0f,%.1f,%.0f,%.1f,%d,%s,%d,%d\n", millis() / 1000, d.values[wu::PH], d.values[wu::ORP],
           d.values[wu::TURB], d.tds25, d.values[wu::TEMP], feMn, stages, d.failSafe, d.alerts);
  f.close();
}

void handleSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == 'T' && state == MONITOR) { state = TREATING; treatStart = millis(); retries = 0; flowPulses = 0; Serial.println("treatment started"); }
    else if (c == 'F') {
      while (!Serial.available()) delay(1);
      char v = Serial.read();
      feMn = (v == '1') ? 1 : (v == '0') ? 0 : -1;
    }
  }
}

void setup() {
  Serial.begin(115200);
  const uint8_t relays[4] = {PIN_RELAY_PUMP, PIN_RELAY_UV, PIN_RELAY_RO, PIN_RELAY_IX};
  for (uint8_t i = 0; i < 4; i++) { pinMode(relays[i], OUTPUT); digitalWrite(relays[i], RELAY_OFF); }
  pinMode(PIN_LED, OUTPUT); pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_LEVEL, INPUT);
  pinMode(PIN_FLOW, INPUT);
  attachInterrupt(digitalPinToInterrupt(PIN_FLOW), onFlowPulse, RISING);
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_PH, ADC_11db); analogSetPinAttenuation(PIN_ORP, ADC_11db);
  analogSetPinAttenuation(PIN_TURB, ADC_11db); analogSetPinAttenuation(PIN_TDS, ADC_11db);
  ds18.begin();
  sdOk = SD.begin(PIN_SD_CS);
  if (sdOk && !SD.exists("/log.csv")) {
    File f = SD.open("/log.csv", FILE_WRITE);
    if (f) { f.println("t_s,ph,orp,turbidity,tds25,temp,fe_mn,stages,fail_safe,alerts"); f.close(); }
  }
  Serial.println(sdOk ? "SD ok" : "SD missing, logging to serial only");
}

void loop() {
  handleSerial();

  if (millis() - lastSample >= SAMPLE_MS) {
    lastSample = millis();
    float raw[wu::N_SENSORS];
    readSensors(raw);
    lastDecision = controller.step(raw, feMn);
    if (lastDecision.alerts) alertBeep(lastDecision.failSafe ? 3 : 1);
    if (++sampleCount >= LOG_EVERY_N_SAMPLES) { sampleCount = 0; logRow(raw, lastDecision); }
  }

  if (state == TREATING) {
    applyStages(lastDecision, true);
    bool levelOk = digitalRead(PIN_LEVEL) == LEVEL_OK_STATE;
    float onSeconds = (millis() - treatStart) / 1000.0f;
    noInterrupts(); uint32_t pulses = flowPulses; interrupts();
    if (wu::pumpGuard(onSeconds, pulses, levelOk) != wu::PUMP_OK) {
      applyStages(lastDecision, false);
      alertBeep(5);
      state = MONITOR;
      Serial.println("pump cut: dry run or empty source");
    } else if (millis() - treatStart >= TREAT_MS) {
      applyStages(lastDecision, false);
      // TODO: the deck's outlet check needs its own sensors and a dispense valve, which
      // are not in the pinout or BOM yet. For now the inlet probes are re-read.
      bool release = !lastDecision.failSafe &&
                     wu::verifyOutlet(lastDecision.values[wu::PH], lastDecision.tds25, lastDecision.values[wu::TURB],
                                      true /* TODO: UV photodiode, not in BOM */);
      digitalWrite(PIN_LED, release ? HIGH : LOW);  // LED stands in for the dispense valve
      if (release) { state = MONITOR; Serial.println("outlet ok, dispense released"); }
      else if (++retries < MAX_RETRIES) { treatStart = millis(); flowPulses = 0; Serial.println("outlet check failed, re-treating"); }
      else { alertBeep(8); state = MONITOR; Serial.println("outlet check failed, service needed"); }
    }
  }
}
