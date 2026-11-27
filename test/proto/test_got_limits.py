"""GOT-limits — behavior at and beyond the axis limits."""
import pytest

from client import DeviceError
from conftest import (assert_angular_close, assert_goto_duration,
                      celestial_dec_at_dec_axis, setup_site_time)


@pytest.fixture(autouse=True)
def _quiet(proto):
    yield
    proto.action("stop")
    proto.control(tracking="none")


def _dec_limit_celestial(proto):
    """Celestial DEC at the +DEC operational limit (effective config)."""
    return celestial_dec_at_dec_axis(proto.state()["limits"]["dec_max"])


def _dec_unreachable(proto):
    """A celestial DEC with no reachable axis solution.

    Both geometries place the axis position at dec_axis = ±(DEC + 90).  A DEC is
    unreachable when the positive side exceeds ``dec_max`` AND the flipped side
    falls below ``dec_min`` — computed from the effective limits, so it holds for
    asymmetric envelopes (BFT's DEC −135..+50) too.
    """
    limits = proto.state()["limits"]
    return max(limits["dec_max"] - 90.0, -limits["dec_min"] - 90.0) + 20.0


def test_goto_beyond_dec_limit_rejected(proto, caps):
    """DEC beyond the reachable range is rejected."""
    setup_site_time(proto)
    assert_goto_duration(proto, caps, 6.0, -60.0)
    with pytest.raises(DeviceError):
        proto.action("goto", ra=6.0, dec=_dec_unreachable(proto), speed=4)
    assert proto.state()["state"] != "slewing"


def test_goto_returns_to_valid_after_rejection(proto, caps):
    """After a rejected out-of-limit goto the mount still slews to valid targets."""
    setup_site_time(proto)
    assert_goto_duration(proto, caps, 6.0, -60.0)
    with pytest.raises(DeviceError):
        proto.action("goto", ra=6.0, dec=_dec_unreachable(proto), speed=4)
    assert_goto_duration(proto, caps, 6.0, -55.0)
    assert proto.state()["state"] != "slewing"


def test_goto_to_exact_dec_limit(proto, caps):
    """A goto to the +DEC operational limit lands exactly at it and succeeds."""
    setup_site_time(proto)
    lst = proto.state()["lst"]
    dec_at_limit = _dec_limit_celestial(proto)
    m = assert_goto_duration(proto, caps, lst, dec_at_limit)
    assert_angular_close(m["final_ra"], m["final_dec"], lst, dec_at_limit, 0.1, "DEC limit")
    assert abs(m["final_dec"] - dec_at_limit) < 0.1, \
        f"DEC final {m['final_dec']} != {dec_at_limit} (limite)"
