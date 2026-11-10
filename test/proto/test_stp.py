"""STP — Stop and cancellation (ACTION stop)."""
import time

import pytest

from conftest import (
    axis_position,
    setup_site_time,
    wait_not_moving,
    wait_slewing,
    wait_tracking,
    wait_until,
)


@pytest.fixture(autouse=True)
def _quiet(proto):
    yield
    proto.action("stop")
    proto.control(tracking="none")


def test_stop_stops_slew(proto):
    """ACTION stop halts an in-progress maneuver."""
    setup_site_time(proto)
    proto.action("goto", ra=6.0, dec=-60.0, speed=4)
    wait_slewing(proto)
    proto.action("stop")
    wait_not_moving(proto)
    assert proto.state()["state"] != "slewing"


def test_stop_cancels_queued_goto(proto):
    """A goto queued behind an active one is cancelled by STOP and never runs."""
    setup_site_time(proto)
    proto.action("goto", ra=6.0, dec=-60.0, speed=4)
    wait_slewing(proto)
    proto.action("goto", ra=7.0, dec=-50.0, speed=4)   # queued
    proto.action("stop")
    wait_not_moving(proto)
    time.sleep(5.0)
    assert proto.state()["state"] != "slewing", "cancelled goto re-appeared"


def test_repeat_stop_when_quiet(proto):
    """Repeating stop while quiet is safe and idempotent."""
    for _ in range(3):
        proto.action("stop")
    assert proto.state()["state"] != "slewing"


def test_move_again_after_stop(proto):
    """After a stop the mount can move again."""
    setup_site_time(proto)
    proto.action("goto", ra=6.0, dec=-60.0, speed=4)
    wait_slewing(proto)
    proto.action("stop")
    wait_not_moving(proto)
    proto.action("goto", ra=6.0, dec=-60.0, speed=4)
    wait_slewing(proto)
    proto.action("stop")
    wait_not_moving(proto)


def test_stop_clears_tracking(proto):
    """ACTION stop (scope all) clears an active tracking."""
    proto.control(tracking="sidereal")
    wait_tracking(proto, "sidereal")
    proto.action("stop")
    assert proto.state()["tracking"] == "none"


def test_stop_manual_preserves_tracking(proto):
    """Stopping a manual move (scope manual) restores the paused tracking."""
    proto.control(tracking="sidereal")
    wait_tracking(proto, "sidereal")
    proto.control(manual_ra_dps=1.0)          # pauses tracking
    proto.action("stop", scope="manual")
    # Tracking is restored asynchronously (the TRACK command is queued), so
    # wait for it rather than asserting on the immediate read.
    wait_tracking(proto, "sidereal")


def test_stop_then_park(proto):
    """Stop followed by park leaves the mount parked."""
    proto.control(tracking="sidereal")
    wait_tracking(proto, "sidereal")
    proto.action("stop")
    proto.action("park")
    assert proto.state()["at_park"] is True
    proto.action("unpark")


def test_stop_marks_position_uncertain(proto):
    """Interrupting a slew mid-batch flags the axis position as uncertain.

    The double-buffered RMT slew loop credits a batch of steps to the internal
    counter only once the whole batch has finished transmitting.  A STOP that
    lands mid-batch therefore drops the already-emitted fraction, so after an
    interrupted move the step counters may no longer match the physical axes.
    The mount must surface that uncertainty rather than pass the counters off
    as a trustworthy absolute position.
    """
    setup_site_time(proto)
    # Establish a physical reference and clear any prior uncertainty.
    proto.action("limits", param="set_home")
    assert proto.state()["position_uncertain"] is False

    proto.action("goto", ra=6.0, dec=-60.0, speed=4)
    wait_slewing(proto)
    # Wait until a batch actually lands (the counters move) so the STOP
    # interrupts an in-flight transmission instead of racing the first batch.
    start = axis_position(proto)
    wait_until(
        lambda: axis_position(proto),
        lambda p: p["ra_steps"] != start["ra_steps"]
                  or p["dec_steps"] != start["dec_steps"],
        10, "axes to move")
    proto.action("stop")
    wait_not_moving(proto)
    assert proto.state()["position_uncertain"] is True

    # A new physical reference re-establishes the position and clears the flag.
    proto.action("limits", param="set_home")
    assert proto.state()["position_uncertain"] is False


def test_home_during_goto(proto):
    """HOME issued mid-slew cancels the goto and drives both axes to (0, 0)."""
    setup_site_time(proto)
    proto.action("goto", ra=6.0, dec=-60.0, speed=4)
    wait_slewing(proto)
    proto.action("home")
    wait_not_moving(proto, timeout=40)
    pos = axis_position(proto)
    assert abs(pos["ra_deg"]) < 1.0, f"RA not home: {pos['ra_deg']}"
    assert abs(pos["dec_deg"]) < 1.0, f"DEC not home: {pos['dec_deg']}"


def test_stop_then_immediate_goto(proto):
    """A goto issued right after STOP (no settle wait) still runs cleanly."""
    setup_site_time(proto)
    proto.action("goto", ra=6.0, dec=-60.0, speed=4)
    wait_slewing(proto)
    proto.action("stop")
    # No wait_not_moving here: STOP is a barrier, so the motion task is already
    # idle when this goto is enqueued.
    proto.action("goto", ra=6.0, dec=-60.0, speed=4)
    wait_slewing(proto)
    proto.action("stop")
    wait_not_moving(proto)
    assert proto.state()["state"] != "slewing"
