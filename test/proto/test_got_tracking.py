"""GOT-tracking — a goto preserves (or leaves off) the tracking state."""
import pytest

from conftest import (
    assert_goto_duration,
    setup_site_time,
    wait_not_moving,
    wait_slewing,
    wait_tracking,
)

MODES = {0: "sidereal", 1: "lunar", 2: "solar"}


@pytest.fixture(autouse=True)
def _no_tracking(proto):
    yield
    proto.control(tracking="none")


def test_goto_preserves_tracking(proto):
    """A goto issued while tracking is active resumes tracking afterwards."""
    setup_site_time(proto)
    proto.control(tracking="sidereal")
    wait_tracking(proto, "sidereal")
    assert_goto_duration(proto, 6.0, -60.0, 6.0)
    assert proto.state()["tracking"] == "sidereal", "goto did not preserve tracking"


def test_goto_without_tracking_stays_off(proto):
    """A goto with tracking off leaves tracking off."""
    setup_site_time(proto)
    assert_goto_duration(proto, 6.0, -60.0, 6.0)
    assert proto.state()["tracking"] == "none"


@pytest.mark.parametrize("rate", [0, 1, 2])
def test_goto_preserves_tracking_rate(proto, rate):
    """A goto preserves both tracking state and the selected rate."""
    setup_site_time(proto)
    proto.control(tracking=MODES[rate])
    wait_tracking(proto, MODES[rate])
    assert_goto_duration(proto, 6.0, -60.0, 6.0)
    assert proto.state()["tracking"] == MODES[rate], \
        f"goto changed rate from {MODES[rate]} to {proto.state()['tracking']}"


def test_goto_stop_cancels_tracking_resume(proto):
    """A STOP that aborts a goto also cancels the tracking resume it carried.

    The resume is intrinsic to the slew command: when the slew completes the
    mount returns to tracking, but when the slew is interrupted the pending
    resume is dropped with it — the mount must not silently restart tracking.
    """
    setup_site_time(proto)
    proto.action("home")
    wait_not_moving(proto)
    proto.control(tracking="sidereal")
    wait_tracking(proto, "sidereal")

    proto.action("goto", ra=6.0, dec=-60.0, speed=4)
    wait_slewing(proto)
    proto.action("stop")
    wait_not_moving(proto)

    assert proto.state()["tracking"] == "none", \
        "an aborted goto must not resume the tracking it paused"
