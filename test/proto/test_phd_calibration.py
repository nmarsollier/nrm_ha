"""PHD2-style guide calibration at real guiding speed.

The whole canonical sequence lives in ONE test so no fixture interleaves a
STOP / home / manual-zero between nominal phases and destroys the state under
test.  The oracle is the NRM-HA geometry (64 microsteps × 300:1 reduction), not
the value reported by the protocol itself.

NRM-HA guide rate default is 0.5 × sidereal (≈0.002089 deg/s), NOT 0.5 deg/s.
The coarse test_guide_effect.py uses 0.5 deg/s only to make direction obvious;
this test asserts the mount actually moves at the real rate and catches an axis
that accepts a pulse yet never steps.

Requires a prepared bench (see test/README.md): a board flashed with this
firmware, a clear sweep, limits that admit DEC=+90°, and identity device_id
"nrm-ha".
"""
import time

import pytest

from conftest import axis_position, setup_site_time, wait_not_moving, wait_tracking

SIDEREAL_DPS = 0.004178074
GUIDE_DPS = SIDEREAL_DPS * 0.5          # 0.002089037 deg/s
DEG_PER_STEP = 1.8 / (64 * 300)         # 0.00009375 deg
ARCSEC_PER_STEP = 0.3375

EXPECTED_500MS_STEPS = GUIDE_DPS * 0.5 / DEG_PER_STEP   # 11.1415307
EXPECTED_12_STEPS = 12 * EXPECTED_500MS_STEPS            # 133.698…

# Discounting the sidereal tracking base leaves a residual equal to the guide
# correction (ra_delta minus the base over the measured interval).  That
# residual carries two uncertainties: step quantization (±1 step) and the
# sidereal drift that elapses during the two state() queries bracketing each
# pulse (~25 ms each → ~2.2 steps), which the elapsed-time base cannot see.
# Bound both; the result still rejects a ~60 % guide rate (~6.7 steps).
SIDEREAL_STEPS_PER_S = SIDEREAL_DPS / DEG_PER_STEP
QUERY_LATENCY_STEPS = SIDEREAL_STEPS_PER_S * 0.05     # ~2.2
PER_PULSE_TOL = 2.0 + QUERY_LATENCY_STEPS              # ~4.2
TRAMO_TOL = 4.0 + QUERY_LATENCY_STEPS                  # ~6.2


def _confirm_identity(proto):
    cap = proto.capabilities()
    assert cap.get("device_id") == "nrm-ha", "wrong board (VID/PID is shared)"
    # boot_id may be a hex string in capabilities and an integer in state.
    st = proto.state()
    assert "boot_id" in st
    return cap


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


def test_phd_calibration_nominal(proto):
    """West → East → DEC backlash-clear/North → South → guide nudges → STOP."""
    _confirm_identity(proto)
    setup_site_time(proto)

    # Reference + a clear bench away from poles and limits.
    proto.action("unpark")
    proto.action("stop")
    proto.action("home")
    wait_not_moving(proto)
    proto.config_set(guide_rate_ra=GUIDE_DPS, guide_rate_dec=GUIDE_DPS)

    proto.action("move", axis="ra", degrees=5.0, speed=4)
    wait_not_moving(proto)
    proto.action("move", axis="dec", degrees=90.0, speed=4)   # DEC mech +90°
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
    assert abs(ra_drift - SIDEREAL_DPS * 2.0 / DEG_PER_STEP) <= 8, \
        "RA baseline off sidereal rate"
    assert dec_drift == 0, "DEC moved during pure tracking"

    # ── West: 12 × 500 ms, each adds ~11.14 steps of RA offset above base ──
    west_residuals = []
    tramo_before = axis_position(proto)["ra_steps"]
    tramo_t0 = time.monotonic()
    for _ in range(12):
        dec_d, ra_d, elapsed = _pulse(proto, "west", 500)
        assert dec_d == 0, "West must not move DEC"
        west_residuals.append(ra_d - SIDEREAL_STEPS_PER_S * elapsed)
        time.sleep(1.0)   # requested inter-pulse pause in the PHD2 sequence
    tramo_after = axis_position(proto)["ra_steps"]
    tramo_elapsed = time.monotonic() - tramo_t0
    west_tramo = (tramo_after - tramo_before) - SIDEREAL_STEPS_PER_S * tramo_elapsed

    for r in west_residuals:
        assert abs(r - EXPECTED_500MS_STEPS) <= PER_PULSE_TOL, \
            f"West residual {r:.1f} != ~{EXPECTED_500MS_STEPS:.1f} steps/pulse"
    assert abs(west_tramo - EXPECTED_12_STEPS) <= TRAMO_TOL, \
        f"West 12-pulse tramo {west_tramo:.1f} != ~{EXPECTED_12_STEPS:.1f} steps"

    # ── East: 12 × 500 ms; each removes ~11.14 steps from the RA base ──
    east_residuals = []
    tramo_before = axis_position(proto)["ra_steps"]
    tramo_t0 = time.monotonic()
    for _ in range(12):
        dec_d, ra_d, elapsed = _pulse(proto, "east", 500)
        assert dec_d == 0, "East must not move DEC"
        assert ra_d > 0, "East must not reverse mechanical RA under tracking"
        east_residuals.append(ra_d - SIDEREAL_STEPS_PER_S * elapsed)
        time.sleep(1.0)
    tramo_after = axis_position(proto)["ra_steps"]
    tramo_elapsed = time.monotonic() - tramo_t0
    east_tramo = (tramo_after - tramo_before) - SIDEREAL_STEPS_PER_S * tramo_elapsed

    for r in east_residuals:
        assert abs(r + EXPECTED_500MS_STEPS) <= PER_PULSE_TOL, \
            f"East residual {r:.1f} != ~{-EXPECTED_500MS_STEPS:.1f} steps/pulse"
    assert abs(east_tramo + EXPECTED_12_STEPS) <= TRAMO_TOL, \
        f"East 12-pulse tramo {east_tramo:.1f} != ~{-EXPECTED_12_STEPS:.1f} steps"

    # ── DEC backlash clear (3 pulses); the 3rd is also North step 1 ──
    for _ in range(2):
        dec_d, _, _ = _pulse(proto, "north", 500)
        assert dec_d > 0, "North must increase mechanical DEC at DEC=+90"
        time.sleep(1.0)
    dec_origin = axis_position(proto)["dec_steps"]

    for _ in range(12):          # 3rd cleanup pulse + 11 more = 12 North
        dec_d, _, _ = _pulse(proto, "north", 500)
        assert dec_d > 0
        time.sleep(1.0)
    north_total = axis_position(proto)["dec_steps"] - dec_origin
    assert abs(north_total - 12 * EXPECTED_500MS_STEPS) <= 3, \
        f"12 North total {north_total} != ~{12 * EXPECTED_500MS_STEPS:.1f}"

    for _ in range(12):
        dec_d, _, _ = _pulse(proto, "south", 500)
        assert dec_d < 0, "South must decrease mechanical DEC at DEC=+90"
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
def test_guide_rate_scale(proto, rate):
    """Guide rate scales the arc linearly at 0.25×/0.5×/1× sidereal."""
    _confirm_identity(proto)
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
    expected = (SIDEREAL_DPS * rate) * 5.0 / DEG_PER_STEP
    assert abs(abs(dec_d) - expected) <= expected * 0.3 + 4, \
        f"rate {rate}×: DEC {dec_d} != ~{expected:.0f}"
    assert abs(dec_d) > 0, f"rate {rate}×: zero movement"
