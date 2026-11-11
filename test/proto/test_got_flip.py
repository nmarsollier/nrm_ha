"""GOT-flip — a goto across the meridian flips the pier side."""
import pytest

from client import DeviceError
from conftest import (assert_angular_close, assert_goto_duration,
                     goto_and_arrive, setup_site_time, wait_not_moving,
                     wait_slewing)

SPEED_DPS = 6.0


def _pier(proto):
    return 0 if proto.state()["pier_side"] == "east" else 1


@pytest.fixture(autouse=True)
def _quiet(proto):
    yield
    proto.action("stop")
    proto.control(tracking="none")


def test_meridian_flip(proto):
    """West-of-meridian → pierEast; east-of-meridian → pierWest (flip)."""
    setup_site_time(proto)
    lst = proto.state()["lst"]

    ra_west = (lst - 4.0) % 24.0   # HA = +4h → pierEast
    assert_goto_duration(proto, ra_west, -60.0, SPEED_DPS)
    assert _pier(proto) == 0, f"HA=+4h should be pierEast, got {_pier(proto)}"

    ra_east = (lst + 4.0) % 24.0   # HA = -4h → pierWest (flip)
    assert_goto_duration(proto, ra_east, -60.0, SPEED_DPS)
    assert _pier(proto) == 1, f"HA=-4h should be pierWest (flip), got {_pier(proto)}"


def test_goto_near_meridian(proto):
    """A target on the meridian (HA≈0) is reached; the side is not forced."""
    setup_site_time(proto)
    lst = proto.state()["lst"]
    ra = lst % 24.0
    m = assert_goto_duration(proto, ra, -60.0, SPEED_DPS)
    assert_angular_close(m["final_ra"], m["final_dec"], ra, -60.0, 0.1, "near-meridian goto")


def test_goto_invalid_side_rejected(proto):
    """A `side` that is present but invalid is rejected, never treated as AUTO."""
    setup_site_time(proto)
    lst = proto.state()["lst"]
    ra = (lst + 4.0) % 24.0   # HA = -4h, a valid reachable target
    dec = -60.0

    with pytest.raises(DeviceError):
        proto.action("goto", ra=ra, dec=dec, side="north")   # invalid value

    with pytest.raises(DeviceError):
        proto.action("goto", ra=ra, dec=dec, side=123)       # invalid JSON type

    assert proto.state()["state"] != "slewing"


def test_goto_flip_interrupted_marks_position_uncertain(proto):
    """Interrupting a flip leaves the position uncertain, not "flip complete"."""
    setup_site_time(proto)
    lst = proto.state()["lst"]

    # Establish WEST (target east of the meridian).
    ra_east = (lst + 4.0) % 24.0   # HA = -4h -> pierWest
    goto_and_arrive(proto, ra_east, -60.0)
    assert _pier(proto) == 1, "setup should be pierWest"

    # Start the flip (target west of the meridian -> pierEast) and stop mid-slew.
    ra_west = (lst - 4.0) % 24.0   # HA = +4h -> flip
    proto.action("goto", ra=ra_west, dec=-60.0, speed=4)
    wait_slewing(proto)
    proto.action("stop")
    wait_not_moving(proto)

    # The interrupted flip must not read as a confident new side.
    assert proto.state()["position_uncertain"] is True
