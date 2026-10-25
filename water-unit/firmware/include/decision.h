// Decision logic for the water unit. Plain C++ with no Arduino calls, so it
// compiles on a laptop (see test/test_decision.cpp) and on the ESP32.
// It mirrors sim/water_unit/decision.py rule for rule.
#pragma once
#include <math.h>
#include <stdint.h>

namespace wu {

enum Sensor : uint8_t { PH = 0, ORP, TURB, TDS, TEMP, N_SENSORS };

struct Config {
  // IS 10500:2012 acceptable limits. Check the standard before relying on them.
  float phMin = 6.5f, phMax = 8.5f;
  float tdsLimit = 500.0f, tdsVeryHigh = 2000.0f;  // mg/L
  float turbLimit = 1.0f, turbHeavy = 100.0f;      // NTU
  float stuckEps = 1e-6f;
  float dryRunSeconds = 5.0f;
};

static const uint8_t MEDIAN_N = 5;
static const uint8_t STUCK_N = 10;

static const float RANGE_LO[N_SENSORS] = {0.0f, -1999.0f, 0.0f, 0.0f, -5.0f};
static const float RANGE_HI[N_SENSORS] = {14.0f, 1999.0f, 3000.0f, 5000.0f, 80.0f};
static const bool STUCK_CHECK[N_SENSORS] = {true, true, true, true, false};

enum FaultKind : uint8_t { OK = 0, MISSING = 1, OUT_OF_RANGE = 2, STUCK = 3 };
enum Alert : uint8_t { A_TDS_VERY_HIGH = 1, A_TURB_HEAVY = 2, A_PH_NOT_CORRECTABLE = 4, A_SENSOR_FAULT = 8 };

struct Stages { bool sediment, carbon, uf, ro, ix, uv; };

struct Decision {
  Stages s;
  FaultKind fault[N_SENSORS];
  uint8_t alerts;
  bool failSafe;
  bool holdDispense;
  float values[N_SENSORS];
  float tds25;
};

inline float compensateTds(float ppm, float tempC) { return ppm / (1.0f + 0.02f * (tempC - 25.0f)); }

// Letters S C U R I V for the enabled stages. out must hold 7 chars.
inline void stageString(const Stages &s, char *out) {
  uint8_t n = 0;
  if (s.sediment) out[n++] = 'S';
  if (s.carbon) out[n++] = 'C';
  if (s.uf) out[n++] = 'U';
  if (s.ro) out[n++] = 'R';
  if (s.ix) out[n++] = 'I';
  if (s.uv) out[n++] = 'V';
  out[n] = 0;
}

class Channel {
 public:
  Channel() : id_(0), nRecent_(0), hRecent_(0), nRaw_(0), hRaw_(0) {}
  void init(uint8_t id) { id_ = id; }

  // Returns a FaultKind. On OK, out holds the median-filtered value.
  FaultKind update(float v, const Config &cfg, float &out) {
    out = NAN;
    if (isnan(v)) return MISSING;
    if (v < RANGE_LO[id_] || v > RANGE_HI[id_]) return OUT_OF_RANGE;
    push(recent_, MEDIAN_N, nRecent_, hRecent_, v);
    push(raw_, STUCK_N, nRaw_, hRaw_, v);
    if (STUCK_CHECK[id_] && nRaw_ == STUCK_N) {
      float mn = raw_[0], mx = raw_[0];
      for (uint8_t i = 1; i < nRaw_; i++) {
        if (raw_[i] < mn) mn = raw_[i];
        if (raw_[i] > mx) mx = raw_[i];
      }
      if (mx - mn <= cfg.stuckEps) return STUCK;
    }
    out = median();
    return OK;
  }

 private:
  static void push(float *buf, uint8_t cap, uint8_t &n, uint8_t &head, float v) {
    buf[head] = v;
    head = (head + 1) % cap;
    if (n < cap) n++;
  }
  float median() const {
    float t[MEDIAN_N];
    for (uint8_t i = 0; i < nRecent_; i++) t[i] = recent_[i];
    for (uint8_t i = 1; i < nRecent_; i++) {  // insertion sort, n <= 5
      float k = t[i];
      int8_t j = i - 1;
      while (j >= 0 && t[j] > k) { t[j + 1] = t[j]; j--; }
      t[j + 1] = k;
    }
    if (nRecent_ % 2) return t[nRecent_ / 2];
    return (t[nRecent_ / 2 - 1] + t[nRecent_ / 2]) / 2.0f;
  }

  uint8_t id_, nRecent_, hRecent_, nRaw_, hRaw_;
  float recent_[MEDIAN_N];
  float raw_[STUCK_N];
};

class Controller {
 public:
  explicit Controller(const Config &cfg = Config()) : cfg_(cfg) {
    for (uint8_t i = 0; i < N_SENSORS; i++) ch_[i].init(i);
  }

  // raw: PH, ORP, TURB, TDS, TEMP (NAN = no reading).
  // feMn: -1 not tested, 0 clear, 1 flagged by the strip test.
  Decision step(const float raw[N_SENSORS], int8_t feMn) {
    Decision d = {};
    bool anyFault = false;
    for (uint8_t i = 0; i < N_SENSORS; i++) {
      d.fault[i] = ch_[i].update(raw[i], cfg_, d.values[i]);
      if (d.fault[i] != OK) anyFault = true;
    }
    d.tds25 = NAN;
    if (anyFault) {
      d.s = {true, true, false, true, true, true};
      d.alerts = A_SENSOR_FAULT;
      d.failSafe = true;
      d.holdDispense = true;
      return d;
    }
    d.tds25 = compensateTds(d.values[TDS], d.values[TEMP]);
    float ph = d.values[PH];
    bool phBad = (ph < cfg_.phMin || ph > cfg_.phMax);
    bool tdsHigh = d.tds25 > cfg_.tdsLimit;
    d.s = {true, true, true, false, false, true};
    d.s.ro = tdsHigh || phBad;
    d.s.uf = !d.s.ro;
    d.s.ix = (feMn == 1);
    if (phBad) d.alerts |= A_PH_NOT_CORRECTABLE;
    if (d.values[TURB] > cfg_.turbHeavy) d.alerts |= A_TURB_HEAVY;
    if (d.tds25 > cfg_.tdsVeryHigh) d.alerts |= A_TDS_VERY_HIGH;
    return d;
  }

 private:
  Config cfg_;
  Channel ch_[N_SENSORS];
};

// True if treated water may be dispensed. Any NAN reading holds it.
inline bool verifyOutlet(float ph, float tds25, float turb, bool uvOk, const Config &cfg = Config()) {
  if (isnan(ph) || isnan(tds25) || isnan(turb)) return false;
  if (ph < cfg.phMin || ph > cfg.phMax) return false;
  if (tds25 > cfg.tdsLimit) return false;
  if (turb > cfg.turbLimit) return false;
  return uvOk;
}

enum PumpState : uint8_t { PUMP_OK = 0, PUMP_CUT_EMPTY = 1, PUMP_CUT_NO_FLOW = 2 };

inline PumpState pumpGuard(float pumpOnSeconds, uint32_t flowPulses, bool levelOk, const Config &cfg = Config()) {
  if (!levelOk) return PUMP_CUT_EMPTY;
  if (pumpOnSeconds >= cfg.dryRunSeconds && flowPulses == 0) return PUMP_CUT_NO_FLOW;
  return PUMP_OK;
}

}  // namespace wu
