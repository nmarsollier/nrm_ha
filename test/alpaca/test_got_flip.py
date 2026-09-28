"""GOT-flip — a goto across the meridian flips the pier side.

With the RA axis limited to ±100° and the home offset at 90°, a target west of
the meridian (HA > ~+10°) is only reachable pierEast, and a target east
(HA < ~-10°) only pierWest.  Crossing the meridian therefore flips the pier.
"""
import pytest

from conftest import assert_goto_duration, assert_angular_close

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


def test_meridian_flip(profile, client):
    """West-of-meridian → pierEast; east-of-meridian → pierWest (flip)."""
    _setup_site_time(client)
    lst = client.get_value("siderealtime")

    # HA = +4h (west) → pierEast
    ra_west = (lst - 4.0) % 24.0
    assert_goto_duration(profile, client, ra_west, -60.0, SPEED_DPS)
    assert client.get_value("sideofpier") == 0, \
        f"HA=+4h should be pierEast, got {client.get_value('sideofpier')}"

    # HA = -4h (east) → pierWest (the flip)
    ra_east = (lst + 4.0) % 24.0
    assert_goto_duration(profile, client, ra_east, -60.0, SPEED_DPS)
    assert client.get_value("sideofpier") == 1, \
        f"HA=-4h should be pierWest (flip), got {client.get_value('sideofpier')}"


def test_goto_near_meridian(profile, client):
    """A target on the meridian (HA≈0) is reached; the pier side is not forced.

    At the transit both mechanical solutions are possible, so unlike the
    HA=±4h cases the limits do not dictate the side.  The goto must still
    arrive at the requested position.
    """
    _setup_site_time(client)
    lst = client.get_value("siderealtime")
    ra = lst % 24.0     # HA ≈ 0
    dec = -60.0
    m = assert_goto_duration(profile, client, ra, dec, SPEED_DPS)
    tol = profile["budgets"]["reported_position_deg"]
    assert_angular_close(m["final_ra"], m["final_dec"], ra, dec, tol, "near-meridian goto")
