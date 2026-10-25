import math
import pytest

from water_unit import Config, Controller, compensate_tds, pump_guard, verify_outlet
from water_unit.scenarios import SCENARIOS, STEPS, dithered

CLEAN = dict(ph=7.2, orp=250, turbidity=0.4, tds=180, temp=28)


def run(base, n=STEPS, fe_mn=None, ctl=None):
    ctl = ctl or Controller()
    d = None
    for i in range(n):
        d = ctl.step(dithered(base, i), fe_mn)
    return d


@pytest.mark.parametrize("name", SCENARIOS)
def test_scenario_stages_and_alerts(name):
    base, fe_mn, stages, alerts = SCENARIOS[name]
    d = run(base, fe_mn=fe_mn)
    assert d.faults == []
    assert not d.fail_safe
    assert d.stage_string() == stages
    assert sorted(d.alerts) == sorted(alerts)


@pytest.mark.parametrize("name", SCENARIOS)
def test_uv_always_on(name):
    base, fe_mn, *_ = SCENARIOS[name]
    assert run(base, fe_mn=fe_mn).stages["uv"]


def test_uf_and_ro_are_exclusive_in_normal_mode():
    for name, (base, fe_mn, *_) in SCENARIOS.items():
        d = run(base, fe_mn=fe_mn)
        assert d.stages["uf"] != d.stages["ro"], name


def test_missing_probe_triggers_fail_safe():
    d = Controller().step(dict(CLEAN, ph=None))
    assert d.fail_safe and d.hold_dispense
    assert d.faults == ["ph:missing"]
    assert d.stage_string() == "SCRIV"
    assert "sensor_fault" in d.alerts


def test_nan_counts_as_missing():
    d = Controller().step(dict(CLEAN, tds=float("nan")))
    assert d.faults == ["tds:missing"]


def test_out_of_range_triggers_fail_safe():
    assert Controller().step(dict(CLEAN, ph=15.0)).faults == ["ph:out_of_range"]
    assert Controller().step(dict(CLEAN, turbidity=-1)).faults == ["turbidity:out_of_range"]


def test_unplugged_ds18b20_reads_minus_127():
    assert Controller().step(dict(CLEAN, temp=-127.0)).faults == ["temp:out_of_range"]


def test_stuck_sensor_flagged_after_n_identical_readings():
    ctl = Controller()
    n = Config().stuck_n
    for i in range(n - 1):
        d = ctl.step(dict(dithered(CLEAN, i), turbidity=5.0))
        assert not d.fail_safe, f"false stuck at step {i}"
    d = ctl.step(dict(dithered(CLEAN, n), turbidity=5.0))
    assert d.faults == ["turbidity:stuck"]
    assert d.fail_safe


def test_constant_temperature_is_not_a_fault():
    ctl = Controller()
    for i in range(40):
        d = ctl.step(dict(dithered(CLEAN, i), temp=28.0))
    assert not d.fail_safe


def test_single_spike_is_filtered_out():
    ctl = Controller()
    for i in range(6):
        ctl.step(dithered(CLEAN, i))
    spike = dict(dithered(CLEAN, 6), ph=3.0)  # one bad sample, still in range
    d = ctl.step(spike)
    assert d.stage_string() == "SCUV"


def test_recovers_after_fault_clears():
    ctl = Controller()
    run(CLEAN, n=8, ctl=ctl)
    assert ctl.step(dict(CLEAN, ph=None)).fail_safe
    assert not ctl.step(dithered(CLEAN, 9)).fail_safe


def test_tds_temperature_compensation():
    assert compensate_tds(500, 25) == pytest.approx(500)
    assert compensate_tds(500, 35) == pytest.approx(500 / 1.2)
    assert compensate_tds(500, 15) == pytest.approx(500 / 0.8)


def test_cold_water_pushes_marginal_tds_over_the_limit():
    warm = run(dict(CLEAN, tds=480, temp=35))
    cold = run(dict(CLEAN, tds=480, temp=15))
    assert not warm.stages["ro"]
    assert cold.stages["ro"]


def test_fe_mn_untested_does_not_run_ion_exchange():
    d = run(CLEAN, fe_mn=None)
    assert not d.stages["ix"]
    assert any("not tested" in r for r in d.reasons)


def test_outlet_release_and_hold():
    assert verify_outlet(7.2, 180, 0.4, True).release
    r = verify_outlet(7.2, 180, 2.0, True)
    assert not r.release and r.reasons == ["turbidity:2.00"]
    assert not verify_outlet(7.2, 180, 0.4, False).release
    assert not verify_outlet(3.2, 900, 0.4, True).release
    r = verify_outlet(None, 180, 0.4, True)
    assert not r.release and r.reasons == ["ph:no_reading"]
    assert not verify_outlet(7.2, math.nan, 0.4, True).release


def test_pump_guard():
    assert pump_guard(2, 0, True) == "ok"          # still priming
    assert pump_guard(5, 0, True) == "cut:no_flow"
    assert pump_guard(5, 40, True) == "ok"
    assert pump_guard(1, 40, False) == "cut:source_empty"
