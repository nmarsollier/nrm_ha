"""LIMITS — a runtime-changed limit is enforced (set and driven via the protocol)."""
import time

import pytest

from client import DeviceError
from conftest import axis_position, wait_not_moving


@pytest.fixture(autouse=True)
def _clear_limits(proto):
    yield
    try:
        proto.action("limits", param="clear_limits")
    except Exception:
        pass


def test_changed_dec_limit_respected(proto):
    """After setting a smaller DEC limit, MoveAxis stops there (not +150°)."""
    proto.action("home")
    wait_not_moving(proto)

    # move to ~+6° and set it as the new DEC right limit
    proto.control(manual_dec_dps=6.0)
    time.sleep(1.0)
    proto.control(manual_dec_dps=0.0)
    wait_not_moving(proto)
    proto.action("limits", param="set_dec_right")
    limit = axis_position(proto)["dec_deg"]

    # move back toward 0
    proto.control(manual_dec_dps=-6.0)
    time.sleep(1.0)
    proto.control(manual_dec_dps=0.0)
    wait_not_moving(proto)

    # move positive must stop at the new limit, not pass it
    proto.control(manual_dec_dps=6.0)
    wait_not_moving(proto, timeout=10)
    pos = axis_position(proto)["dec_deg"]
    assert pos <= limit + 0.5, f"DEC passed the changed limit: {pos} > {limit}"


def test_limit_set_rejects_inverted_range(proto):
    """Setting a limit that would make min >= max is rejected, not stored."""
    proto.action("home")
    wait_not_moving(proto)

    # ra_min := current position (0°), leaving ra_max at the default 100°.
    proto.action("limits", param="set_ra_left")

    # ra_max := 0° would make ra_min (0) >= ra_max (0) — must be rejected and
    # leave the previous valid config untouched.
    with pytest.raises(DeviceError):
        proto.action("limits", param="set_ra_right")

    st = proto.state()
    assert st["limits"]["ra_min"] == pytest.approx(0.0)
    assert st["limits"]["ra_max"] == pytest.approx(100.0)
