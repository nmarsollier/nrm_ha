"""GOT-limits — behavior at and beyond the axis limits.

DEC axis limit is ±150°; dec_axis = DEC + 90 (pierEast) or -(DEC + 90)
(pierWest).  A target whose only axis solutions fall outside the limits must be
rejected without moving; a valid target must still be reachable afterwards.
"""
import pytest

from conftest import assert_angular_close, assert_goto_duration, wait_not_slewing

SPEED_DPS = 6.0
TOL = 0.1


def _setup_site_time(client):
    client.put_ok("utcdate", form={"UTCDate": "2026-09-28T02:00:00"})
    client.put_ok("sitelatitude", form={"SiteLatitude": "-32.89"})
    client.put_ok("sitelongitude", form={"SiteLongitude": "-68.83"})


@pytest.fixture(autouse=True)
def _quiet(client):
    yield
    client.put_ok("abortslew")
    client.put_ok("tracking", form={"Tracking": "false"})


def test_goto_beyond_dec_limit_rejected(profile, client):
    """DEC beyond the reachable range (dec_axis > ±150°) is rejected."""
    _setup_site_time(client)
    assert_goto_duration(profile, client, 6.0, -60.0, SPEED_DPS)

    # DEC = +80° → dec_axis = ±170°, both outside ±150°.
    r = client.put("slewtocoordinatesasync",
                   form={"RightAscension": "6.0", "Declination": "80.0"})
    assert r.error_number != 0, "goto a DEC=80° fue aceptado"
    assert client.get_value("slewing") is False, "rejected goto left slewing"


def test_goto_returns_to_valid_after_rejection(profile, client):
    """After a rejected out-of-limit goto the mount still slews to valid targets."""
    _setup_site_time(client)
    assert_goto_duration(profile, client, 6.0, -60.0, SPEED_DPS)

    r = client.put("slewtocoordinatesasync",
                   form={"RightAscension": "6.0", "Declination": "80.0"})
    assert r.error_number != 0

    # a normal goto still works
    assert_goto_duration(profile, client, 6.0, -55.0, SPEED_DPS)
    assert client.get_value("slewing") is False


def test_goto_to_exact_dec_limit(profile, client):
    """A goto to DEC=+60° lands at the DEC axis limit (±150°) and succeeds."""
    _setup_site_time(client)
    lst = client.get_value("siderealtime")
    # RA = LST → HA = 0 (on the meridian); DEC=+60 → dec_axis = ±150 (the limit)
    m = assert_goto_duration(profile, client, lst, 60.0, SPEED_DPS)
    assert_angular_close(m["final_ra"], m["final_dec"], lst, 60.0, TOL, "DEC=+60")
    # reported DEC is at the reachable limit
    assert abs(m["final_dec"] - 60.0) < TOL, \
        f"DEC final {m['final_dec']} != +60 (limite)"
