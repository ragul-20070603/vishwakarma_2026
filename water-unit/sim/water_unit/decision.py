"""Decision logic for the smart water purification unit.

Pure Python, no hardware imports. The same rules are ported to C++ in
firmware/include/decision.h, and both are checked against the same scenarios.

Pipeline per cycle (slide 4 of the deck):
    read -> validate (range + stuck) -> median filter -> compare -> select stages

Rules in short:
  * Sediment and carbon are always in the water path. UV-C is always on.
  * UF is the default membrane path. The RO path is used instead when
    temperature-compensated TDS is above the limit, or when pH is outside the
    acceptable band (dissolved contaminants are likely and nothing else in the
    train can handle them).
  * Ion exchange runs only when the Fe/Mn strip test flags iron or manganese.
  * Any sensor fault (missing, out of range, stuck) puts the unit in fail-safe:
    full train, dispensing held, alert raised.
"""
from __future__ import annotations

import math
from collections import deque
from dataclasses import dataclass, field
from statistics import median
from typing import Optional

SENSORS = ("ph", "orp", "turbidity", "tds", "temp")
STAGES = ("sediment", "carbon", "uf", "ro", "ix", "uv")
STAGE_LETTERS = {"sediment": "S", "carbon": "C", "uf": "U", "ro": "R", "ix": "I", "uv": "V"}

# Physically plausible range per sensor. Readings outside it are treated as a
# fault (disconnected probe, shorted wire, ADC rail). Set these to the real
# range of the probes you buy.
RANGES = {
    "ph": (0.0, 14.0),
    "orp": (-1999.0, 1999.0),
    "turbidity": (0.0, 3000.0),
    "tds": (0.0, 5000.0),
    "temp": (-5.0, 80.0),  # a DS18B20 that is unplugged reads -127, which lands here
}

# Stuck-value check applies to these only. Water temperature can sit on the same
# DS18B20 step for many minutes, so it would give false faults.
STUCK_CHECKED = ("ph", "orp", "turbidity", "tds")

# What this unit cannot see. The dashboard should always show this.
NOT_MEASURED = (
    "bacteria and viruses (UV-C is the only barrier)",
    "arsenic",
    "lead",
    "cadmium",
    "fluoride",
    "iron and manganese, except by the manual strip test",
)


@dataclass(frozen=True)
class Config:
    # IS 10500:2012 acceptable limits as I remember them. Check the standard
    # itself before you rely on any of these numbers.
    ph_min: float = 6.5
    ph_max: float = 8.5
    tds_limit: float = 500.0       # mg/L
    tds_very_high: float = 2000.0  # mg/L, alert only
    turbidity_limit: float = 1.0   # NTU
    turbidity_heavy: float = 100.0  # NTU, alert only (clogging risk). Tunable.
    median_n: int = 5
    stuck_n: int = 10
    stuck_eps: float = 1e-6
    dry_run_s: float = 5.0


class Channel:
    """One sensor: range check, stuck check, median filter."""

    def __init__(self, name: str, cfg: Config):
        self.name = name
        self.cfg = cfg
        self.recent: deque = deque(maxlen=cfg.median_n)
        self.raw: deque = deque(maxlen=cfg.stuck_n)

    def update(self, value: Optional[float]):
        """Return (filtered_value, fault). filtered_value is None on a fault."""
        if value is None or (isinstance(value, float) and math.isnan(value)):
            return None, "missing"
        lo, hi = RANGES[self.name]
        if not (lo <= value <= hi):
            return None, "out_of_range"
        self.recent.append(value)
        self.raw.append(value)
        if (
            self.name in STUCK_CHECKED
            and len(self.raw) == self.cfg.stuck_n
            and max(self.raw) - min(self.raw) <= self.cfg.stuck_eps
        ):
            return None, "stuck"
        return median(self.recent), None


@dataclass
class Decision:
    stages: dict
    faults: list
    alerts: list
    reasons: list
    fail_safe: bool
    hold_dispense: bool
    values: dict

    def stage_string(self) -> str:
        return "".join(STAGE_LETTERS[s] for s in STAGES if self.stages[s])


def compensate_tds(tds_ppm: float, temp_c: float) -> float:
    """Normalise a TDS reading to 25 C (about 2 % per degree)."""
    return tds_ppm / (1.0 + 0.02 * (temp_c - 25.0))


class Controller:
    def __init__(self, cfg: Config = Config()):
        self.cfg = cfg
        self.channels = {n: Channel(n, cfg) for n in SENSORS}

    def step(self, raw: dict, fe_mn: Optional[bool] = None) -> Decision:
        """Run one cycle. raw maps sensor name to a value (None if no reading).
        fe_mn is the strip-test result: True flagged, False clear, None not tested."""
        values, faults = {}, []
        for name in SENSORS:
            v, fault = self.channels[name].update(raw.get(name))
            values[name] = v
            if fault:
                faults.append(f"{name}:{fault}")
        if faults:
            return self._fail_safe(values, faults)

        cfg = self.cfg
        tds25 = compensate_tds(values["tds"], values["temp"])
        values["tds25"] = tds25
        ph = values["ph"]
        stages = {s: False for s in STAGES}
        stages["sediment"] = stages["carbon"] = stages["uv"] = True
        reasons, alerts = [], []

        ph_bad = not (cfg.ph_min <= ph <= cfg.ph_max)
        tds_high = tds25 > cfg.tds_limit
        if tds_high:
            reasons.append(f"TDS {tds25:.0f} mg/L above {cfg.tds_limit:.0f}: RO path")
        if ph_bad:
            reasons.append(f"pH {ph:.2f} outside {cfg.ph_min}-{cfg.ph_max}: RO path")
            alerts.append("ph_not_correctable")
        stages["ro"] = tds_high or ph_bad
        stages["uf"] = not stages["ro"]

        if values["turbidity"] > cfg.turbidity_limit:
            reasons.append(f"turbidity {values['turbidity']:.1f} NTU: sediment + membrane")
        if values["turbidity"] > cfg.turbidity_heavy:
            alerts.append("turbidity_heavy")
        if tds25 > cfg.tds_very_high:
            alerts.append("tds_very_high")

        if fe_mn:
            stages["ix"] = True
            reasons.append("Fe/Mn flagged by strip test: ion exchange")
        elif fe_mn is None:
            reasons.append("Fe/Mn not tested this cycle")

        return Decision(stages, [], alerts, reasons, False, False, values)

    def _fail_safe(self, values, faults) -> Decision:
        stages = {"sediment": True, "carbon": True, "uf": False, "ro": True, "ix": True, "uv": True}
        return Decision(
            stages=stages,
            faults=faults,
            alerts=["sensor_fault"],
            reasons=["sensor fault: full treatment, dispensing held"],
            fail_safe=True,
            hold_dispense=True,
            values=values,
        )


@dataclass
class OutletResult:
    release: bool
    reasons: list = field(default_factory=list)


def verify_outlet(ph, tds25, turbidity, uv_ok: bool, cfg: Config = Config()) -> OutletResult:
    """Decide whether treated water may be dispensed. Any missing reading holds it."""
    missing = [
        f"{n}:no_reading"
        for n, v in (("ph", ph), ("tds", tds25), ("turbidity", turbidity))
        if v is None or (isinstance(v, float) and math.isnan(v))
    ]
    if missing:
        return OutletResult(False, missing)
    reasons = []
    if not (cfg.ph_min <= ph <= cfg.ph_max):
        reasons.append(f"ph:{ph:.2f}")
    if tds25 > cfg.tds_limit:
        reasons.append(f"tds:{tds25:.0f}")
    if turbidity > cfg.turbidity_limit:
        reasons.append(f"turbidity:{turbidity:.2f}")
    if not uv_ok:
        reasons.append("uv:lamp_not_confirmed")
    return OutletResult(not reasons, reasons)


def pump_guard(pump_on_s: float, flow_pulses: int, level_ok: bool, cfg: Config = Config()) -> str:
    """'ok', or the reason the pump must be cut."""
    if not level_ok:
        return "cut:source_empty"
    if pump_on_s >= cfg.dry_run_s and flow_pulses == 0:
        return "cut:no_flow"
    return "ok"
