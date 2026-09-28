"""GOT-sites — the same goto resolves correctly from different site coordinates.

The site lat/long feed LST (and hence hour angle and pier side), so the same
celestial target maps to different axis positions at different sites.  The goto
must still reach the reported target each time.
"""
import pytest

from conftest import assert_angular_close, assert_goto_duration

SPEED_DPS = 6.0
TOL = 0.1


@pytest.fixture(autouse=True)
def _quiet(client):
    yield
    client.put_ok("abortslew")
    client.put_ok("tracking", form={"Tracking": "false"})


@pytest.mark.parametrize("lat,lon", [
    ("-32.89", "-68.83"),   # southern, home site
    ("-23.55", "-46.63"),   # southern, different longitude
    ("40.71", "-74.01"),    # northern
])
def test_goto_across_sites(profile, client, lat, lon):
    client.put_ok("utcdate", form={"UTCDate": "2026-09-28T02:00:00"})
    client.put_ok("sitelatitude", form={"SiteLatitude": lat})
    client.put_ok("sitelongitude", form={"SiteLongitude": lon})
    m = assert_goto_duration(profile, client, 6.0, -60.0, SPEED_DPS)
    assert_angular_close(m["final_ra"], m["final_dec"], 6.0, -60.0, TOL,
                         f"site {lat}/{lon}")
