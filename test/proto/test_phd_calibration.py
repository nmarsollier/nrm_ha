"""PHD2-style guide calibration at real guiding speed.

The whole canonical sequence lives in ONE test so no fixture interleaves a
STOP / home / manual-zero between nominal phases and destroys the state under
test.  The expected step counts are computed from the mechanical scale the
mount declares via CAPABILITIES (full-step angle × microsteps × reduction) and
the sidereal rate — never from a hardcoded per-mount value.

The mount's default guide rate is 0.5 × sidereal (≈0.002089 deg/s), NOT
0.5 deg/s.  The coarse test_guide_effect.py uses 0.5 deg/s only to make
direction obvious; this test asserts the mount actually moves at the real rate
and catches an axis that accepts a pulse yet never steps.
"""
import time

import pytest

from conftest import (axis_position, deg_per_step, setup_site_time,
                      wait_not_moving, wait_tracking)

SIDEREAL_DPS = 0.004178074


def _pulse(proto, direction, duration_ms):
    """Issue one guide pulse; validate acceptance, start, expiry and tracking
    preservation; return (dec_delta_steps, ra_delta_steps, elapsed_s).

    `elapsed_s` is the wall-clock window between the position reads before and
    after the pulse, so the caller can discount the sidereal tracking base and
    isolate the guide correction."""
    st = proto.state()
    assert st["state"] == "tracking", f"expected tracking, got {st['state']}"
    assert st["tracking"] == "sidereal"
    assert st["guiding"] is False, "guiding must be false before a pulse"

    t0 = time.monotonic()
    before = axis_position(proto)
    proto.action("guide", direction=direction, duration_ms=duration_ms)

    # The first guiding=false right after the ACK does not prove completion —
    # the pulse may not have started.  Wait for a start, then for an end.
    start_deadline = time.monotonic() + 1.0
    while time.monotonic() < start_deadline and not proto.state()["guiding"]:
        time.sleep(0.01)
    assert proto.state()["guiding"] is True, "pulse never started"

    end_deadline = time.monotonic() + duration_ms / 1000.0 + 1.0
    while time.monotonic() < end_deadline and proto.state()["guiding"]:
        time.sleep(0.01)
    assert proto.state()["guiding"] is False, "pulse did not end"

    st = proto.state()
    assert st["tracking"] == "sidereal", "tracking lost during pulse"
    assert st["state"] == "tracking", f"state={st['state']} after pulse"
    assert st.get("power") is not False

    after = axis_position(proto)
    elapsed = time.monotonic() - t0
    return after["dec_steps"] - before["dec_steps"], \
        after["ra_steps"] - before["ra_steps"], \
        elapsed


@pytest.fixture(autouse=True)
def _quiet(proto):
    yield
    proto.action("stop")
    proto.control(tracking="none")


def test_phd_calibration_nominal(proto, caps):
    """West → East → DEC backlash-clear/North → South → guide nudges → STOP."""
    dps = deg_per_step(caps)
    guide_dps = SIDEREAL_DPS * 0.5                  # 0.002089 deg/s
    expected_500ms = guide_dps * 0.5 / dps           # steps per 500 ms pulse
    expected_12 = 12 * expected_500ms
    sidereal_steps_per_s = SIDEREAL_DPS / dps
    query_latency = sidereal_steps_per_s * 0.05     # ~2.2
    per_pulse_tol = 2.0 + query_latency
    tramo_tol = 4.0 + query_latency

    setup_site_time(proto)

    # Reference + a clear bench away from poles and limits.  The DEC bench angle
    # is the smaller of 90° and (effective DEC ceiling − 10°), so it is valid on
    # both mounts.
    proto.action("unpark")
    proto.action("stop")
    proto.action("home")
    wait_not_moving(proto)
    proto.config_set(guide_rate_ra=guide_dps, guide_rate_dec=guide_dps)

    bench_dec = min(90.0, proto.state()["limits"]["dec_max"] - 10.0)
    proto.action("move", axis="ra", degrees=5.0, speed=4)
    wait_not_moving(proto)
    proto.action("move", axis="dec", degrees=bench_dec, speed=4)
    wait_not_moving(proto)

    proto.control(tracking="sidereal")
    wait_tracking(proto, "sidereal")

    # ── Baseline: RA advances at sidereal, DEC stays still ──
    b0 = axis_position(proto)
    time.sleep(2.0)
    b1 = axis_position(proto)
    ra_drift = b1["ra_steps"] - b0["ra_steps"]
    dec_drift = b1["dec_steps"] - b0["dec_steps"]
    assert ra_drift > 0, "RA did not advance at sidereal"
    assert abs(ra_drift - SIDEREAL_DPS * 2.0 / dps) <= 8, \
        "RA baseline off sidereal rate"
    assert dec_drift == 0, "DEC moved during pure tracking"

    # ── West: 12 × 500 ms, each adds ~11 steps of RA offset above base ──
    west_residuals = []
    tramo_before = axis_position(proto)["ra_steps"]
    tramo_t0 = time.monotonic()
    for _ in range(12):
        dec_d, ra_d, elapsed = _pulse(proto, "west", 500)
        assert dec_d == 0, "West must not move DEC"
        west_residuals.append(ra_d - sidereal_steps_per_s * elapsed)
        time.sleep(1.0)   # requested inter-pulse pause in the PHD2 sequence
    tramo_after = axis_position(proto)["ra_steps"]
    tramo_elapsed = time.monotonic() - tramo_t0
    west_tramo = (tramo_after - tramo_before) - sidereal_steps_per_s * tramo_elapsed

    for r in west_residuals:
        assert abs(r - expected_500ms) <= per_pulse_tol, \
            f"West residual {r:.1f} != ~{expected_500ms:.1f} steps/pulse"
    assert abs(west_tramo - expected_12) <= tramo_tol, \
        f"West 12-pulse tramo {west_tramo:.1f} != ~{expected_12:.1f} steps"

    # ── East: 12 × 500 ms; each removes ~11 steps from the RA base ──
    east_residuals = []
    tramo_before = axis_position(proto)["ra_steps"]
    tramo_t0 = time.monotonic()
    for _ in range(12):
        dec_d, ra_d, elapsed = _pulse(proto, "east", 500)
        assert dec_d == 0, "East must not move DEC"
        assert ra_d > 0, "East must not reverse mechanical RA under tracking"
        east_residuals.append(ra_d - sidereal_steps_per_s * elapsed)
        time.sleep(1.0)
    tramo_after = axis_position(proto)["ra_steps"]
    tramo_elapsed = time.monotonic() - tramo_t0
    east_tramo = (tramo_after - tramo_before) - sidereal_steps_per_s * tramo_elapsed

    for r in east_residuals:
        assert abs(r + expected_500ms) <= per_pulse_tol, \
            f"East residual {r:.1f} != ~{-expected_500ms:.1f} steps/pulse"
    assert abs(east_tramo + expected_12) <= tramo_tol, \
        f"East 12-pulse tramo {east_tramo:.1f} != ~{-expected_12:.1f} steps"

    # ── DEC backlash clear (3 pulses); the 3rd is also North step 1 ──
    for _ in range(2):
        dec_d, _, _ = _pulse(proto, "north", 500)
        assert dec_d > 0, "North must increase mechanical DEC on the positive-D side"
        time.sleep(1.0)
    dec_origin = axis_position(proto)["dec_steps"]

    for _ in range(12):          # 3rd cleanup pulse + 11 more = 12 North
        dec_d, _, _ = _pulse(proto, "north", 500)
        assert dec_d > 0
        time.sleep(1.0)
    north_total = axis_position(proto)["dec_steps"] - dec_origin
    assert abs(north_total - 12 * expected_500ms) <= 3, \
        f"12 North total {north_total} != ~{12 * expected_500ms:.1f}"

    for _ in range(12):
        dec_d, _, _ = _pulse(proto, "south", 500)
        assert dec_d < 0, "South must decrease mechanical DEC on the positive-D side"
        time.sleep(1.0)
    dec_final = axis_position(proto)["dec_steps"] - dec_origin
    assert abs(dec_final) <= 3, f"12 South did not return to DEC origin ({dec_final})"

    # ── Post-calibration nudges: mixed directions and durations ──
    for direction, ms in [("north", 250), ("east", 750),
                          ("south", 250), ("west", 750),
                          ("north", 1000), ("south", 1000)]:
        _pulse(proto, direction, ms)

    # ── Explicit STOP ends everything cleanly ──
    proto.action("stop", scope="all")
    st = proto.state()
    assert st["guiding"] is False
    assert st["tracking"] == "none"
    assert st["state"] in ("ready", "parked")


@pytest.mark.parametrize("rate", [0.25, 0.5, 1.0])
def test_guide_rate_scale(proto, caps, rate):
    """Guide rate scales the arc linearly at 0.25×/0.5×/1× sidereal."""
    setup_site_time(proto)
    proto.action("home")
    wait_not_moving(proto)
    proto.action("move", axis="dec", degrees=30.0, speed=4)
    wait_not_moving(proto)
    proto.config_set(guide_rate_dec=SIDEREAL_DPS * rate)

    before = axis_position(proto)
    proto.action("guide", direction="north", duration_ms=5000)
    time.sleep(5.5)
    after = axis_position(proto)
    dec_d = after["dec_steps"] - before["dec_steps"]
    expected = (SIDEREAL_DPS * rate) * 5.0 / deg_per_step(caps)
    assert abs(abs(dec_d) - expected) <= expected * 0.3 + 4, \
        f"rate {rate}×: DEC {dec_d} != ~{expected:.0f}"
    assert abs(dec_d) > 0, f"rate {rate}×: zero movement"
