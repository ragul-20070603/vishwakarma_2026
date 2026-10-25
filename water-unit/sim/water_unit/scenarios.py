"""Test scenarios shared by the tests, the simulator and the C++ test.

dither(i) is deterministic on purpose so the C++ test can reproduce it exactly.
"""
import random

# name -> (readings, fe_mn, expected stage string, expected alerts)
SCENARIOS = {
    "clean_tap":      (dict(ph=7.2, orp=250, turbidity=0.4, tds=180,  temp=28), None, "SCUV",  []),
    "muddy_pond":     (dict(ph=7.4, orp=230, turbidity=85,  tds=220,  temp=28), None, "SCUV",  []),
    "very_muddy":     (dict(ph=7.4, orp=230, turbidity=150, tds=220,  temp=28), None, "SCUV",  ["turbidity_heavy"]),
    "mine_drainage":  (dict(ph=3.2, orp=520, turbidity=12,  tds=1400, temp=28), None, "SCRV",  ["ph_not_correctable"]),
    "salty_borewell": (dict(ph=7.8, orp=200, turbidity=0.8, tds=900,  temp=28), None, "SCRV",  []),
    "iron_well":      (dict(ph=7.0, orp=120, turbidity=3,   tds=300,  temp=28), True, "SCUIV", []),
}

STEPS = 30


def dither(i: int) -> int:
    """-2..2 repeating pattern. Same formula in C++."""
    return ((i * 3) % 5) - 2


def dithered(base: dict, i: int) -> dict:
    return {k: v + (v * 0.01 + 0.01) * dither(i) for k, v in base.items()}


def noisy(base: dict, rng: random.Random) -> dict:
    """Gaussian noise for the simulator and the report."""
    out = {}
    for k, v in base.items():
        if k == "ph":
            s = 0.03
        elif k == "orp":
            s = 3.0
        elif k == "turbidity":
            s = 0.02 + 0.02 * v
        elif k == "tds":
            s = 0.015 * v
        else:
            s = 0.05
        out[k] = v + rng.gauss(0, s)
        if k != "orp":
            out[k] = max(0.0, out[k])
    return out
