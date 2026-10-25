"""Run every scenario through the decision engine and write a CSV log.

    cd sim && python simulate.py

Output: ../data/sim_log.csv and a summary table on the screen. The readings are
synthetic (base value plus noise). This shows the logic working, not real water.
"""
import csv
import random
from pathlib import Path

from water_unit import Controller, STAGES
from water_unit.scenarios import SCENARIOS, STEPS, noisy

OUT = Path(__file__).resolve().parent.parent / "data" / "sim_log.csv"
FIELDS = ["scenario", "step", "ph", "orp", "turbidity", "tds_raw", "tds_25", "temp", "fe_mn",
          *STAGES, "stages", "fail_safe", "hold_dispense", "faults", "alerts"]


def run_scenario(name, base, fe_mn, seed=1, fault=None):
    """fault: optional (step, sensor, value) injected into the raw stream."""
    rng = random.Random(seed)
    ctl = Controller()
    rows = []
    for i in range(STEPS):
        raw = noisy(base, rng)
        if fault and i >= fault[0]:
            raw[fault[1]] = fault[2]
        d = ctl.step(raw, fe_mn)
        v = d.values
        rows.append({
            "scenario": name, "step": i,
            "ph": raw["ph"], "orp": raw["orp"], "turbidity": raw["turbidity"],
            "tds_raw": raw["tds"], "tds_25": v.get("tds25"), "temp": raw["temp"],
            "fe_mn": "" if fe_mn is None else int(fe_mn),
            **{s: int(d.stages[s]) for s in STAGES},
            "stages": d.stage_string(), "fail_safe": int(d.fail_safe),
            "hold_dispense": int(d.hold_dispense),
            "faults": ";".join(d.faults), "alerts": ";".join(d.alerts),
        })
    return rows


def main():
    jobs = [(n, b, f, None) for n, (b, f, *_rest) in SCENARIOS.items()]
    clean = SCENARIOS["clean_tap"][0]
    jobs.append(("probe_unplugged", clean, None, (10, "ph", None)))
    jobs.append(("turbidity_stuck", clean, None, (5, "turbidity", 0.4)))

    all_rows = []
    print(f"{'scenario':<18}{'final stages':<14}{'fail-safe':<11}alerts / faults")
    for name, base, fe_mn, fault in jobs:
        rows = run_scenario(name, base, fe_mn, fault=fault)
        all_rows += rows
        last = rows[-1]
        extra = ";".join(x for x in (last["alerts"], last["faults"]) if x) or "-"
        print(f"{name:<18}{last['stages']:<14}{'YES' if last['fail_safe'] else 'no':<11}{extra}")

    OUT.parent.mkdir(exist_ok=True)
    with OUT.open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=FIELDS)
        w.writeheader()
        for r in all_rows:
            w.writerow({k: (round(v, 3) if isinstance(v, float) else v) for k, v in r.items()})
    print(f"\nwrote {OUT} ({len(all_rows)} rows)")


if __name__ == "__main__":
    main()
