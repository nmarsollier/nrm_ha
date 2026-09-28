"""MAN — Manual centering and movement (MoveAxis)."""
import time

import pytest

from conftest import wait_slewing, wait_not_slewing, axis_position


@pytest.fixture(autouse=True)
def _stop(client):
    yield
    client.put_ok("moveaxis", form={"Axis": "0", "Rate": "0.0"})
    client.put_ok("moveaxis", form={"Axis": "1", "Rate": "0.0"})
    client.put_ok("abortslew")


def _move(client, axis, rate):
    client.put_ok("moveaxis", form={"Axis": str(axis), "Rate": str(rate)})


# ── manual ───
def test_move_axis_constant_speed(client):
    """MoveAxis moves an axis at constant speed."""
    _move(client, 0, 1.0)
    wait_slewing(client)
    _move(client, 0, 0.0)
    wait_not_slewing(client)
    assert client.get_value("slewing") is False


# ── manual ───
def test_move_axis_reverse(profile, client):
    """Reversing the direction actually reverses the RA motion."""
    _move(client, 0, 1.0)
    wait_slewing(client)
    time.sleep(0.3)
    before = axis_position(profile)["ra_deg"]

    _move(client, 0, -1.0)   # invertir
    time.sleep(1.0)
    after = axis_position(profile)["ra_deg"]

    assert after < before, \
        f"RA did not reverse: before={before:.3f}° after={after:.3f}°"
    _move(client, 0, 0.0)
    wait_not_slewing(client)


# ── manual ───
def test_move_two_axes_stop_one(profile, client):
    """Stopping one axis preserves the other's motion."""
    _move(client, 0, 1.0)
    _move(client, 1, 1.0)
    wait_slewing(client)
    _move(client, 0, 0.0)   # stop only RA; DEC keeps going
    time.sleep(0.3)
    dec0 = axis_position(profile)["dec_deg"]
    time.sleep(0.6)
    dec1 = axis_position(profile)["dec_deg"]
    assert abs(dec1 - dec0) > 0.1, \
        f"DEC stopped when RA was stopped (Δ={dec1 - dec0:.3f}°)"
    _move(client, 1, 0.0)
    wait_not_slewing(client)


# ── manual ───
def test_release_axis_repeatedly(client):
    """Releasing the control (Rate=0) repeatedly is idempotent."""
    _move(client, 0, 1.0)
    wait_slewing(client)
    for _ in range(3):
        client.put_ok("moveaxis", form={"Axis": "0", "Rate": "0.0"})
    assert client.get_value("slewing") is False


# ── manual ───
def test_change_rate_without_release(client):
    """Change the speed without releasing the control."""
    _move(client, 0, 1.0)
    wait_slewing(client)
    _move(client, 0, 3.0)   # raise the rate
    time.sleep(0.3)
    assert client.get_value("slewing") is True
    _move(client, 0, 0.0)
    wait_not_slewing(client)


# ── manual — limits ───
@pytest.mark.parametrize("axis,limit", [(0, 100.0), (1, 150.0)])
def test_move_axis_limit(profile, client, axis, limit):
    """MoveAxis stops at the axis limit, never passes it, and moves back."""
    client.put_ok("findhome")
    wait_not_slewing(client)

    # toward the positive limit
    _move(client, axis, 6.0)
    wait_not_slewing(client, timeout=40)
    pos = axis_position(profile)
    val = pos["ra_deg"] if axis == 0 else pos["dec_deg"]
    assert val <= limit + 0.1, f"axis {axis} passed the limit: {val}"

    # at the limit: a further move toward it does not pass
    _move(client, axis, 6.0)
    wait_not_slewing(client, timeout=5)

    # away from the limit
    _move(client, axis, -6.0)
    wait_slewing(client)
    _move(client, axis, 0.0)
    wait_not_slewing(client)
    pos2 = axis_position(profile)
    val2 = pos2["ra_deg"] if axis == 0 else pos2["dec_deg"]
    assert val2 < limit, f"axis {axis} did not move away from the limit"
