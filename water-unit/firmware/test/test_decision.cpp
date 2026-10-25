// Native test for decision.h. Build and run on a laptop:
//   g++ -std=c++11 -Wall -Wextra -I include test/test_decision.cpp -o /tmp/wu_test && /tmp/wu_test
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include "decision.h"

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL line %d: %s\n", __LINE__, #cond); failures++; } } while (0)

static int dither(int i) { return ((i * 3) % 5) - 2; }

static void dithered(const float base[wu::N_SENSORS], int i, float out[wu::N_SENSORS]) {
  for (int k = 0; k < wu::N_SENSORS; k++) out[k] = base[k] + (base[k] * 0.01f + 0.01f) * dither(i);
}

struct Scenario { const char *name; float base[wu::N_SENSORS]; int8_t feMn; const char *stages; uint8_t alerts; };
// order: ph, orp, turbidity, tds, temp   (same table as sim/water_unit/scenarios.py)
static const Scenario SCEN[] = {
  {"clean_tap",      {7.2f, 250, 0.4f, 180, 28}, -1, "SCUV",  0},
  {"muddy_pond",     {7.4f, 230, 85,   220, 28}, -1, "SCUV",  0},
  {"very_muddy",     {7.4f, 230, 150,  220, 28}, -1, "SCUV",  wu::A_TURB_HEAVY},
  {"mine_drainage",  {3.2f, 520, 12,  1400, 28}, -1, "SCRV",  wu::A_PH_NOT_CORRECTABLE},
  {"salty_borewell", {7.8f, 200, 0.8f, 900, 28}, -1, "SCRV",  0},
  {"iron_well",      {7.0f, 120, 3,    300, 28},  1, "SCUIV", 0},
};

static wu::Decision runScenario(const float base[wu::N_SENSORS], int8_t feMn, wu::Controller &c, int steps = 30) {
  wu::Decision d = {};
  float raw[wu::N_SENSORS];
  for (int i = 0; i < steps; i++) { dithered(base, i, raw); d = c.step(raw, feMn); }
  return d;
}

int main() {
  char buf[8];
  for (unsigned s = 0; s < sizeof(SCEN) / sizeof(SCEN[0]); s++) {
    wu::Controller c;
    wu::Decision d = runScenario(SCEN[s].base, SCEN[s].feMn, c);
    wu::stageString(d.s, buf);
    if (strcmp(buf, SCEN[s].stages) != 0 || d.alerts != SCEN[s].alerts || d.failSafe) {
      printf("FAIL scenario %s: got %s alerts=%d failSafe=%d\n", SCEN[s].name, buf, d.alerts, d.failSafe);
      failures++;
    }
    CHECK(d.s.uv);
  }

  const float CLEAN[wu::N_SENSORS] = {7.2f, 250, 0.4f, 180, 28};
  float raw[wu::N_SENSORS];

  { // missing probe
    wu::Controller c; dithered(CLEAN, 0, raw); raw[wu::PH] = NAN;
    wu::Decision d = c.step(raw, -1);
    wu::stageString(d.s, buf);
    CHECK(d.failSafe && d.holdDispense && d.fault[wu::PH] == wu::MISSING && strcmp(buf, "SCRIV") == 0);
    CHECK(d.alerts & wu::A_SENSOR_FAULT);
  }
  { // out of range, and unplugged DS18B20
    wu::Controller c; dithered(CLEAN, 0, raw); raw[wu::PH] = 15.0f;
    CHECK(c.step(raw, -1).fault[wu::PH] == wu::OUT_OF_RANGE);
    wu::Controller c2; dithered(CLEAN, 0, raw); raw[wu::TEMP] = -127.0f;
    CHECK(c2.step(raw, -1).fault[wu::TEMP] == wu::OUT_OF_RANGE);
  }
  { // stuck turbidity: fault on the 10th identical reading, not before
    wu::Controller c; wu::Decision d = {};
    for (int i = 0; i < 9; i++) { dithered(CLEAN, i, raw); raw[wu::TURB] = 5.0f; d = c.step(raw, -1); CHECK(!d.failSafe); }
    dithered(CLEAN, 9, raw); raw[wu::TURB] = 5.0f; d = c.step(raw, -1);
    CHECK(d.fault[wu::TURB] == wu::STUCK && d.failSafe);
  }
  { // constant temperature is fine
    wu::Controller c; wu::Decision d = {};
    for (int i = 0; i < 40; i++) { dithered(CLEAN, i, raw); raw[wu::TEMP] = 28.0f; d = c.step(raw, -1); }
    CHECK(!d.failSafe);
  }
  { // one spike is filtered out
    wu::Controller c;
    for (int i = 0; i < 6; i++) { dithered(CLEAN, i, raw); c.step(raw, -1); }
    dithered(CLEAN, 6, raw); raw[wu::PH] = 3.0f;
    wu::Decision d = c.step(raw, -1); wu::stageString(d.s, buf);
    CHECK(strcmp(buf, "SCUV") == 0);
  }
  { // temperature compensation moves a marginal TDS reading across the limit
    float warm[wu::N_SENSORS] = {7.2f, 250, 0.4f, 480, 35}, cold[wu::N_SENSORS] = {7.2f, 250, 0.4f, 480, 15};
    wu::Controller a, b;
    CHECK(!runScenario(warm, -1, a).s.ro);
    CHECK(runScenario(cold, -1, b).s.ro);
    CHECK(fabsf(wu::compensateTds(500, 35) - 500.0f / 1.2f) < 0.01f);
  }
  { // outlet check and pump guard
    CHECK(wu::verifyOutlet(7.2f, 180, 0.4f, true));
    CHECK(!wu::verifyOutlet(7.2f, 180, 2.0f, true));
    CHECK(!wu::verifyOutlet(7.2f, 180, 0.4f, false));
    CHECK(!wu::verifyOutlet(NAN, 180, 0.4f, true));
    CHECK(wu::pumpGuard(2, 0, true) == wu::PUMP_OK);
    CHECK(wu::pumpGuard(5, 0, true) == wu::PUMP_CUT_NO_FLOW);
    CHECK(wu::pumpGuard(1, 40, false) == wu::PUMP_CUT_EMPTY);
  }

  if (failures) { printf("%d check(s) failed\n", failures); return 1; }
  printf("all C++ checks passed\n");
  return 0;
}
