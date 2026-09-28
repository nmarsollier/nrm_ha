"""PRK — Park, unpark and session close."""
import pytest

from conftest import wait_not_slewing


@pytest.fixture(autouse=True)
def _unpark(client):
    yield
    client.put_ok("unpark")


# ── park ───
def test_park_from_observation(client):
    """Parking from observation stops the mount and sets AtPark."""
    client.put_ok("tracking", form={"Tracking": "true"})
    client.put_ok("park")
    assert client.get_value("atpark") is True
    assert client.get_value("tracking") is False


# ── park ───
def test_park_again(client):
    """Parking an already-parked mount is idempotent."""
    client.put_ok("park")
    assert client.get_value("atpark") is True
    client.put_ok("park")
    assert client.get_value("atpark") is True


# ── park ───
def test_parked_blocks_motion(client):
    """While parked, movements are rejected."""
    client.put_ok("park")
    r = client.put("slewtocoordinatesasync",
                   form={"RightAscension": "6.0", "Declination": "-60.0"})
    assert r.error_number != 0, "GOTO estando parked fue aceptado"


# ── park ───
def test_parked_rejects_all_commands(client):
    """While parked only unpark is allowed; every other command is rejected."""
    client.put_ok("park")
    rejected = [
        ("slewtocoordinatesasync", {"RightAscension": "6.0", "Declination": "-60.0"}),
        ("moveaxis", {"Axis": "0", "Rate": "1.0"}),
        ("moveaxis", {"Axis": "1", "Rate": "1.0"}),
        ("findhome", {}),
        ("pulseguide", {"Direction": "0", "Duration": "100"}),
        ("tracking", {"Tracking": "true"}),
        ("slewtotargetasync", {}),
    ]
    for name, form in rejected:
        r = client.put(name, form=form)
        assert r.error_number != 0, f"{name} accepted while parked"

    # unpark is the only command that is allowed
    client.put_ok("unpark")
    assert client.get_value("atpark") is False


# ── park ───
def test_unpark(client):
    """Unparking returns to normal operation."""
    client.put_ok("park")
    assert client.get_value("atpark") is True
    client.put_ok("unpark")
    assert client.get_value("atpark") is False
    # now it can move
    r = client.put("slewtocoordinatesasync",
                   form={"RightAscension": "6.0", "Declination": "-60.0"})
    assert r.error_number == 0, "after unpark the GOTO was rejected"
    client.put_ok("abortslew")
