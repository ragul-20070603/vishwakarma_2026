"""Draw the stage matrix and a fail-safe timeline from data/sim_log.csv.

    cd sim && python simulate.py && python report.py

Output: ../docs/stage_matrix.png and ../docs/failsafe_timeline.png
"""
import csv
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

ROOT = Path(__file__).resolve().parent.parent
STAGES = ["sediment", "carbon", "uf", "ro", "ix", "uv"]
LABELS = ["Sediment", "Carbon", "UF", "RO", "Ion exch.", "UV-C"]

rows = list(csv.DictReader((ROOT / "data" / "sim_log.csv").open()))
names = list(dict.fromkeys(r["scenario"] for r in rows))
final = {n: [r for r in rows if r["scenario"] == n][-1] for n in names}

fig, ax = plt.subplots(figsize=(8, 4.2))
for y, n in enumerate(names):
    for x, s in enumerate(STAGES):
        on = final[n][s] == "1"
        ax.add_patch(plt.Rectangle((x, y), 0.92, 0.82, color="#1f77b4" if on else "#e6e6e6"))
    if final[n]["fail_safe"] == "1":
        ax.text(len(STAGES) + 0.1, y + 0.4, "FAIL-SAFE", va="center", color="#c0392b", fontsize=9)
ax.set_xlim(0, len(STAGES) + 1.3)
ax.set_ylim(0, len(names))
ax.set_xticks([i + 0.46 for i in range(len(STAGES))])
ax.set_xticklabels(LABELS, fontsize=9)
ax.xaxis.tick_top()
ax.set_yticks([i + 0.41 for i in range(len(names))])
ax.set_yticklabels(names, fontsize=9)
ax.invert_yaxis()
ax.set_title("Stages selected per scenario (simulated readings)", pad=28, fontsize=11)
for sp in ax.spines.values():
    sp.set_visible(False)
ax.tick_params(length=0)
fig.tight_layout()
fig.savefig(ROOT / "docs" / "stage_matrix.png", dpi=150)

# fail-safe timeline: pH probe unplugged at step 10
r = [x for x in rows if x["scenario"] == "probe_unplugged"]
steps = [int(x["step"]) for x in r]
fig, (a1, a2) = plt.subplots(2, 1, figsize=(8, 4), sharex=True, gridspec_kw={"height_ratios": [2, 1]})
a1.plot(steps, [float(x["ph"]) if x["ph"] not in ("", "None") else float("nan") for x in r], marker="o", ms=3)
a1.axvline(10, color="#c0392b", ls="--")
a1.text(10.3, 0.85, "pH probe unplugged", color="#c0392b", fontsize=8, transform=a1.get_xaxis_transform())
a1.set_ylabel("pH reading")
a2.step(steps, [int(x["fail_safe"]) for x in r], where="post", color="#c0392b")
a2.set_yticks([0, 1]); a2.set_yticklabels(["normal", "fail-safe"], fontsize=8)
a2.set_xlabel("cycle")
fig.suptitle("Sensor fault puts the unit in fail-safe (full train, dispensing held)", fontsize=10)
fig.tight_layout()
fig.savefig(ROOT / "docs" / "failsafe_timeline.png", dpi=150)
print("wrote docs/stage_matrix.png and docs/failsafe_timeline.png")
