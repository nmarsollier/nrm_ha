"""GOT — Apuntado a objetos reales."""
import pytest

from conftest import (wait_not_slewing, wait_slewing, assert_angular_close,
                      assert_goto_duration)

SPEED_DPS = 6.0


def _setup_site_time(client):
    client.put_ok("utcdate", form={"UTCDate": "2026-09-28T02:00:00"})
    client.put_ok("sitelatitude", form={"SiteLatitude": "-32.89"})
    client.put_ok("sitelongitude", form={"SiteLongitude": "-68.83"})


@pytest.fixture(autouse=True)
def _quiet(client):
    yield
    client.put_ok("abortslew")
    client.put_ok("tracking", form={"Tracking": "false"})


def _goto_and_arrive(client, ra, dec):
    client.put_ok("slewtocoordinatesasync",
                  form={"RightAscension": str(ra), "Declination": str(dec)})
    return wait_not_slewing(client)


# ── slewing / goto ───
def test_goto_from_rest(profile, client):
    """GOTO to an object from rest reaches the reported destination."""
    _setup_site_time(client)
    ra, dec = 6.0, -60.0
    m = assert_goto_duration(profile, client, ra, dec, SPEED_DPS)
    tol = profile["budgets"]["reported_position_deg"]
    assert_angular_close(m["final_ra"], m["final_dec"], ra, dec, tol, "slew from rest")


# ── slewing / goto ───
def test_target_and_slew_to_target(profile, client):
    """TargetRA/TargetDEC + SlewToTarget."""
    _setup_site_time(client)
    ra, dec = 8.0, -45.0
    client.put_ok("targetrightascension", form={"TargetRightAscension": str(ra)})
    client.put_ok("targetdeclination", form={"TargetDeclination": str(dec)})
    assert client.get_value("targetrightascension") == ra
    assert client.get_value("targetdeclination") == dec

    client.put_ok("slewtotargetasync")
    final = wait_not_slewing(client)
    tol = profile["budgets"]["reported_position_deg"]
    assert_angular_close(final["ra"], final["dec"], ra, dec, tol, "apuntado a objetivo")


# ── slewing / goto ───
def test_goto_to_current_position(client):
    """GOTO to the current position causes no excursion."""
    _setup_site_time(client)
    ra = client.get_value("rightascension")
    dec = client.get_value("declination")
    _goto_and_arrive(client, ra, dec)
    assert client.get_value("slewing") is False


# ── slewing / goto ───
def test_goto_impossible(profile, client):
    """GOTO to forbidden coordinates is rejected without moving."""
    _setup_site_time(client)

    r = client.put("slewtocoordinatesasync",
                   form={"RightAscension": "25.0", "Declination": "0.0"})
    assert r.error_number != 0, "GOTO con RA=25 aceptado"

    # no movement started (celestial RA/DEC drifts with sidereal time)
    assert client.get_value("slewing") is False, "rejected GOTO started slewing"


# ── slewing / goto ───
def test_goto_during_goto(client):
    """A new GOTO during another GOTO leaves no false state."""
    _setup_site_time(client)
    client.put_ok("slewtocoordinatesasync",
                  form={"RightAscension": "6.0", "Declination": "-60.0"})
    wait_slewing(client)
    # second GOTO while the first is still running
    client.put_ok("slewtocoordinatesasync",
                  form={"RightAscension": "8.0", "Declination": "-45.0"})
    final = wait_not_slewing(client)
    # ends in a coherent state (not slewing)
    assert final["slewing"] is False
