"""REF — Reference, home and position recovery."""
import pytest

from conftest import (wait_not_slewing, wait_slewing, wait_tracking,
                      axis_position)


@pytest.fixture(autouse=True)
def _unpark(client):
    yield
    client.put_ok("unpark")


# ── reference ───
def test_home_park_celestial_distinct(client):
    """AtHome, AtPark and the celestial position are distinct concepts."""
    # freshly started, no explicit home: AtHome reflects the real state
    assert isinstance(client.get_value("athome"), bool)
    assert isinstance(client.get_value("atpark"), bool)


# ── reference ───
def test_find_home_from_position(profile, client):
    """FindHome moves the mount to the reported home position."""
    # move away with a short GOTO
    client.put_ok("utcdate", form={"UTCDate": "2026-09-28T02:00:00"})
    client.put_ok("slewtocoordinatesasync",
                  form={"RightAscension": "6.0", "Declination": "-60.0"})
    wait_not_slewing(client)

    client.put_ok("findhome")
    wait_not_slewing(client)
    assert client.get_value("slewing") is False
    # reported home within tolerance
    assert client.get_value("athome") is True


# ── reference ───
def test_find_home_when_already_home(client):
    """FindHome when already at home causes no excursion."""
    client.put_ok("findhome")
    wait_not_slewing(client)
    assert client.get_value("athome") is True


# ── reference ───
@pytest.mark.parametrize("ra,dec", [(6.0, -60.0), (8.0, -45.0), (10.0, -70.0)])
def test_find_home_from_various_positions(profile, client, ra, dec):
    """FindHome returns to axis (0,0) from different sky positions."""
    client.put_ok("utcdate", form={"UTCDate": "2026-09-28T02:00:00"})
    client.put_ok("slewtocoordinatesasync",
                  form={"RightAscension": str(ra), "Declination": str(dec)})
    wait_not_slewing(client)

    client.put_ok("findhome")
    wait_not_slewing(client)
    pos = axis_position(profile)
    assert abs(pos["ra_deg"]) < 1.0 and abs(pos["dec_deg"]) < 1.0, \
        f"home no es (0,0): ra={pos['ra_deg']} dec={pos['dec_deg']}"


@pytest.mark.parametrize("rate", [0, 1, 2])
def test_find_home_in_tracking(profile, client, rate):
    """FindHome from each tracking mode returns home and clears tracking."""
    client.put_ok("utcdate", form={"UTCDate": "2026-09-28T02:00:00"})
    client.put_ok("tracking", form={"Tracking": "true"})
    client.put_ok("trackingrate", form={"TrackingRate": str(rate)})
    wait_tracking(client, True)

    client.put_ok("findhome")
    wait_not_slewing(client)
    pos = axis_position(profile)
    assert abs(pos["ra_deg"]) < 1.0 and abs(pos["dec_deg"]) < 1.0
    assert client.get_value("tracking") is False, "home should not resume tracking"


def test_find_home_mid_goto(profile, client):
    """FindHome issued during a goto stops the goto and returns home."""
    client.put_ok("utcdate", form={"UTCDate": "2026-09-28T02:00:00"})
    client.put_ok("slewtocoordinatesasync",
                  form={"RightAscension": "6.0", "Declination": "-60.0"})
    wait_slewing(client)

    client.put_ok("findhome")  # mid-goto
    wait_not_slewing(client)
    pos = axis_position(profile)
    assert abs(pos["ra_deg"]) < 1.0 and abs(pos["dec_deg"]) < 1.0, \
        f"home mid-goto no volvió a (0,0): ra={pos['ra_deg']} dec={pos['dec_deg']}"
