"""CFG — Site, time and coordinates."""
import time

import pytest


@pytest.fixture(autouse=True)
def _quiet(client):
    yield
    client.put_ok("abortslew")
    client.put_ok("tracking", form={"Tracking": "false"})


# ── configuration ───
def test_set_site(profile, client):
    """Set the observing site and read it back."""
    client.put_ok("sitelatitude", form={"SiteLatitude": "-32.89"})
    client.put_ok("sitelongitude", form={"SiteLongitude": "-68.83"})
    client.put_ok("siteelevation", form={"SiteElevation": "750"})

    assert abs(client.get_value("sitelatitude") - (-32.89)) < 0.01
    assert abs(client.get_value("sitelongitude") - (-68.83)) < 0.01
    assert client.get_value("siteelevation") == 750


# ── configuration ───
def test_set_utc_date(client):
    """Set UTC date and read it back."""
    client.put_ok("utcdate", form={"UTCDate": "2026-09-28T02:00:00"})
    got = client.get_value("utcdate")
    assert got.startswith("2026-09-28"), f"UTCDate inesperada: {got}"


# ── configuration ───
def test_sidereal_time_coherent(client):
    """Reported sidereal time is numeric and advances with time."""
    client.put_ok("utcdate", form={"UTCDate": "2026-09-28T02:00:00"})
    lst1 = client.get_value("siderealtime")
    time.sleep(1.1)
    lst2 = client.get_value("siderealtime")
    assert 0 <= lst1 < 24
    # sidereal time advances (the 24h wrap is respected)
    assert (lst2 - lst1) % 24 > 0, f"LST did not advance: {lst1} -> {lst2}"


# ── configuration ───
def test_change_site_during_motion(profile, client):
    """Changing site/time during a maneuver produces no partial effect."""
    client.put_ok("utcdate", form={"UTCDate": "2026-09-28T02:00:00"})
    client.put_ok("slewtocoordinatesasync",
                  form={"RightAscension": "6.0", "Declination": "-60.0"})

    # while the maneuver is in progress, a site change with an invalid field
    # is rejected without touching the previous configuration
    r = client.put("sitelatitude", form={"SiteLatitude": "abc"})
    assert r.error_number != 0, "invalid latitude accepted"
    assert abs(client.get_value("sitelatitude") - (-32.89)) < 0.01

    client.put_ok("abortslew")
