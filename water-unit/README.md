# Smart water purification and quality monitoring unit: software prototype

Vishwakarma 2026 submission, solar-powered unit that reads water quality offline and switches on only the purification stages the water needs.

This repo is the software side of the project: the decision logic, a simulator that runs it against test water, and a firmware sketch for the ESP32. The hardware is not built yet (the fabrication plan runs Nov 2026 to Feb 2027), so every reading in `data/` is synthetic. It shows the logic working. It does not show real water being treated.

## Where things stand

| Piece | State |
| --- | --- |
| Decision logic in Python | Done. 26 tests pass |
| Same logic in C++ for the ESP32 | Done. Native test passes against the same scenarios |
| Scenario simulator, CSV log, plots | Done, on synthetic readings |
| ESP32 sketch (`firmware/src/main.cpp`) | Written to the slide 7 pinout. Not compiled, not run on hardware |
| Probe calibration constants | Placeholders. Need buffer solutions and bench data |
| Fe/Mn strip reader (TCS3200) | Not written. A serial command stands in for the result |
| GSM, Wi-Fi and LoRa sync | Not written |
| Web dashboard | Not written. Only the static plots in `docs/` exist |
| Hardware | Not built |

## Run it

```
pip install -r requirements.txt

cd sim
python -m pytest -q              # 26 tests
python simulate.py               # writes ../data/sim_log.csv
python report.py                 # writes ../docs/*.png

cd ../firmware
g++ -std=c++11 -Wall -Wextra -I include test/test_decision.cpp -o /tmp/wu_test && /tmp/wu_test
```

To flash the sketch, open `firmware/` in PlatformIO and upload. Expect to fix compile errors on the first try, since it has only been checked by reading.

## How it decides

Each cycle reads pH, ORP, turbidity, TDS and temperature, then:

1. Checks every reading for a missing value, an out-of-range value, or a stuck value (the same number for 10 cycles in a row). Temperature skips the stuck check because a DS18B20 can sit on one step for a long time.
2. Median-filters the last 5 good readings, so one bad sample cannot flip a decision.
3. Corrects TDS to 25 C, since conductivity-based TDS drifts about 2% per degree.
4. Picks stages.

Sediment, carbon and UV-C are always on. UF is the default membrane path. The RO path replaces it when corrected TDS is above 500 mg/L, or when pH is outside 6.5 to 8.5. Ion exchange runs only when the Fe/Mn strip test is flagged.

If any sensor faults, the unit runs everything (sediment, carbon, RO, ion exchange, UV-C), holds dispensing and raises an alert. When a reading is doubtful it treats more, not less.

Treated water is released only if pH, TDS and turbidity at the outlet are inside limits and the UV lamp is confirmed. A missing reading holds it. The pump is cut if the source is empty or no flow shows up within 5 seconds.

## Simulated results

| Scenario | Stages | Notes |
| --- | --- | --- |
| clean_tap | Sediment, carbon, UF, UV | |
| muddy_pond | Sediment, carbon, UF, UV | |
| very_muddy (150 NTU) | Sediment, carbon, UF, UV | Alert: clogging risk |
| mine_drainage (pH 3.2, TDS 1400) | Sediment, carbon, RO, UV | Alert: pH cannot be corrected |
| salty_borewell (TDS 900) | Sediment, carbon, RO, UV | |
| iron_well (Fe/Mn flagged) | Sediment, carbon, UF, ion exchange, UV | |
| probe_unplugged | Everything | Fail-safe, dispensing held |
| turbidity_stuck | Everything | Fail-safe, dispensing held |

Plots: `docs/stage_matrix.png` and `docs/failsafe_timeline.png`.

## Open problems

These are things the design still has to answer, not bugs.

- Nothing in the train corrects pH. For acid mine drainage the unit sends water down the RO path, raises an alert, and the outlet check will probably hold it. A neutralising stage (limestone or calcite bed, for example) would close this.
- The outlet check needs its own sensors and a dispense valve, and the UV check needs a photodiode. None of the three are in the slide 7 pinout or the BOM yet.
- The IS 10500 limits in `decision.py` were written from memory. Check them against the standard before quoting them anywhere.
- ORP is read, validated and logged, but no decision uses it yet. It needs lab data before it can.
- The stuck-value window (10 cycles) should be set from real probe noise on the bench. Too short and a quiet probe gives false faults.
- Many hobby TDS probes top out around 1000 ppm. If yours does, the "very high TDS" alert at 2000 can never fire.
- Not measured at all: bacteria and viruses (UV-C is the only barrier), arsenic, lead, cadmium, fluoride. Iron and manganese are covered only by the manual strip test.

## Layout

```
sim/water_unit/decision.py    the rules
sim/water_unit/scenarios.py   shared test scenarios
sim/tests/                    pytest suite
sim/simulate.py, report.py    CSV log and plots
firmware/include/decision.h   the same rules in C++
firmware/test/                native C++ test
firmware/src/main.cpp         ESP32 sketch
data/sim_log.csv              synthetic log
docs/                         plots
```
