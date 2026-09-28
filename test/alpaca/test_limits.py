"""LIMITS — runtime-changed limits are still enforced and reset cleanly.

The complete limit map is spread across the suite: goto (test_got_limits.py),
MoveAxis (test_man.py), tracking (test_trk.py) and PulseGuide
(test_guide_effect.py).  This file covers the remaining case — changing a limit
at runtime and proving the new boundary is respected.
"""
import time

import pytest

from conftest import axis_position, rest_post, wait_not_slewing, wait_slewing


@pytest.fixture(autouse=True)
def _quiet(profile, client):
    yield
    client.put_ok("abortslew")
    client.put_ok("tracking", form={"Tracking": "false"})
    rest_post(profile, "/api/limits", {"action": "clear_limits"})


def _move_dec_to(profile, client, target_deg):
    """Move the DEC axis toward target_deg and stop once reached (positive dir)."""
    client.put_ok("moveaxis", form={"Axis": "1", "Rate": "6.0"})
    deadline = time.monotonic() + target_deg / 6.0 + 5.0
    while time.monotonic() < deadline:
        if axis_position(profile)["dec_deg"] >= target_deg - 0.5:
            break
        time.sleep(0.2)
    client.put_ok("moveaxis", form={"Axis": "1", "Rate": "0.0"})
    wait_not_slewing(client)


def test_changed_dec_limit_respected(profile, client):
    """After setting a smaller DEC limit, MoveAxis stops there (not +150°)."""
    client.put_ok("findhome")
    wait_not_slewing(client)

    # move to ~+6° and set it as the new DEC right limit
    _move_dec_to(profile, client, 6.0)
    rest_post(profile, "/api/limits", {"action": "set_dec_right"})
    limit = axis_position(profile)["dec_deg"]

    # move DEC back toward 0
    client.put_ok("moveaxis", form={"Axis": "1", "Rate": "-6.0"})
    wait_slewing(client)
    time.sleep(1.0)
    client.put_ok("moveaxis", form={"Axis": "1", "Rate": "0.0"})
    wait_not_slewing(client)

    # move-axis positive must stop at the new limit, not pass it
    client.put_ok("moveaxis", form={"Axis": "1", "Rate": "6.0"})
    wait_not_slewing(client, timeout=10)
    pos = axis_position(profile)["dec_deg"]
    assert pos <= limit + 0.5, f"DEC passed the changed limit: {pos} > {limit}"
