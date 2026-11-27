"""GOTO across the meridian on a fork mount (no meridian flip).

Fork-specific: crossing the meridian does not flip the pier side.  The mount
picks the reachable minimum-travel axis solution instead of the flipped
equivalent when that equivalent falls outside the RA travel range.
"""
import pytest

from conftest import (assert_angular_close, assert_goto_duration,
                      setup_site_time, wait_not_moving, wait_tracking)

pytestmark = pytest.mark.geometry("fork")


@pytest.fixture(autouse=True)
def _quiet(proto):
    yield
    proto.action("stop")
    proto.control(tracking="none")


def test_meridian_crossing_uses_min_travel(proto, caps):
    """Crossing the meridian picks the reachable minimum-travel position.

    For HA=+4h the unflipped solution (R=+60, D=+30) is closest from home.  For
    HA=-4h the flipped equivalent (R=+120, D=-30) is equidistant, so it is only
    bypassed when +120 falls outside the RA envelope — a precondition computed
    from the effective limits rather than assumed.
    """
    setup_site_time(proto)
    lst = proto.state()["lst"]
    ra_max = proto.state()["limits"]["ra_max"]

    ra_west = (lst - 4.0) % 24.0   # HA = +4h -> unflipped (R=+60, D=+30)
    assert_goto_duration(proto, caps, ra_west, -60.0)
    assert abs(proto.state()["dec_axis_deg"] - 30) < 0.1
    assert 59 < proto.state()["ra_axis_deg"] < 61

    ra_east = (lst + 4.0) % 24.0   # HA = -4h -> flipped equivalent R=+120
    assert_goto_duration(proto, caps, ra_east, -60.0)
    assert ra_max < 120.0, \
        "flipped equivalent R=+120 is inside the RA envelope; test scenario does not apply"
    assert abs(proto.state()["dec_axis_deg"] - 30) < 0.1
    assert -61 < proto.state()["ra_axis_deg"] < -59


def test_goto_near_meridian(proto, caps):
    """A target on the meridian (HA≈0) is reached; the side is not forced."""
    setup_site_time(proto)
    lst = proto.state()["lst"]
    ra = lst % 24.0
    m = assert_goto_duration(proto, caps, ra, -60.0)
    assert_angular_close(m["final_ra"], m["final_dec"], ra, -60.0, 0.1, "near-meridian goto")


@pytest.mark.parametrize("tracking", ["none", "sidereal", "lunar", "solar"])
def test_same_object_across_meridian(proto, tracking):
    """GOTO the same object before/after the meridian keeps the orientation
    (no flip; pier_side stays unknown)."""
    setup_site_time(proto)
    proto.action("home")
    wait_not_moving(proto)
    ra = (proto.state()["lst"] + 2.0) % 24.0
    proto.action("goto", ra=ra, dec=-60.0, speed=4)
    wait_not_moving(proto)
    before = proto.state()
    assert -31 < before["ra_axis_deg"] < -29
    assert abs(before["dec_axis_deg"] - 30) < 0.1
    assert before["pier_side"] == "unknown"
    proto.control(tracking=tracking)
    wait_tracking(proto, tracking)

    proto.config_set(utc="2026-09-28T06:00:00Z")
    proto.action("goto", ra=ra, dec=-60.0, speed=4)
    wait_not_moving(proto)
    after = proto.state()
    assert 29 < after["ra_axis_deg"] < 32
    assert abs(after["dec_axis_deg"] - before["dec_axis_deg"]) < 0.1
    assert after["tracking"] == tracking
    assert after["pier_side"] == "unknown"
    assert_angular_close(after["ra"], after["dec"], ra, -60, 0.1)
