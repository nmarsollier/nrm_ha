"""GOT-sequence — start-position header, consecutive gotos, NINA-style correction."""
import time

import pytest

from conftest import (assert_angular_close, assert_goto_duration, axis_position,
                      wait_not_slewing)

SPEED_DPS = 6.0
TOL = 0.1  # reported_position_deg budget


def _setup_site_time(client):
    client.put_ok("utcdate", form={"UTCDate": "2026-09-28T02:00:00"})
    client.put_ok("sitelatitude", form={"SiteLatitude": "-32.89"})
    client.put_ok("sitelongitude", form={"SiteLongitude": "-68.83"})


@pytest.fixture(autouse=True)
def _quiet(client):
    yield
    client.put_ok("abortslew")
    client.put_ok("tracking", form={"Tracking": "false"})


def test_goto_reports_start_position(profile, client):
    """The goto response header carries the axis position at command receipt."""
    _setup_site_time(client)
    start = axis_position(profile)
    r = client.put_ok("slewtocoordinatesasync",
                      form={"RightAscension": "6.0", "Declination": "-60.0"})
    snap = r.snapshot()
    assert snap is not None, "no X-NRM-Snapshot header on goto"
    assert abs(snap["ra"] - start["ra_steps"]) <= 100, \
        f"snapshot ra {snap['ra']} != start {start['ra_steps']} (±1 batch)"
    assert abs(snap["dec"] - start["dec_steps"]) <= 100
    wait_not_slewing(client)


def test_consecutive_gotos(profile, client):
    """A chain of gotos each reaches its target and takes the expected time."""
    _setup_site_time(client)
    for ra, dec in [(6.0, -60.0), (8.0, -45.0), (10.0, -70.0), (6.0, -55.0)]:
        m = assert_goto_duration(profile, client, ra, dec, SPEED_DPS)
        assert_angular_close(m["final_ra"], m["final_dec"], ra, dec, TOL,
                             f"goto {ra}h/{dec}°")


def test_nina_style_goto_then_correct(profile, client):
    """N.I.N.A. pattern: a large goto followed by a small correction."""
    _setup_site_time(client)
    # first a long goto
    assert_goto_duration(profile, client, 6.0, -60.0, SPEED_DPS)
    # then a small correction (a few degrees) — triangular ramp
    r = assert_goto_duration(profile, client, 6.0, -62.0, SPEED_DPS)
    assert r["ra_dist"] + r["dec_dist"] < 5.0, "correction was not small"


def test_goto_from_home(profile, client):
    """A goto started from home begins at axis (0,0) and reaches its target."""
    _setup_site_time(client)
    client.put_ok("findhome")
    wait_not_slewing(client)
    start = axis_position(profile)
    assert abs(start["ra_deg"]) < 1.0 and abs(start["dec_deg"]) < 1.0, \
        f"home is not axis (0,0): ra={start['ra_deg']} dec={start['dec_deg']}"
    m = assert_goto_duration(profile, client, 6.0, -60.0, SPEED_DPS)
    assert_angular_close(m["final_ra"], m["final_dec"], 6.0, -60.0, TOL, "from home")


def test_nina_correct_at_dec_limit(profile, client):
    """NINA-style correction near the DEC axis limit stays valid."""
    _setup_site_time(client)
    lst = client.get_value("siderealtime")
    # near the limit (DEC=+58 → dec_axis=±148), then a small correction
    assert_goto_duration(profile, client, lst, 58.0, SPEED_DPS)
    assert_goto_duration(profile, client, lst, 55.0, SPEED_DPS)


def test_nina_correct_across_meridian(profile, client):
    """A correction crossing the meridian flips the pier side."""
    _setup_site_time(client)
    lst = client.get_value("siderealtime")
    # just west (HA=+1h) → pierEast
    ra_w = (lst - 1.0) % 24.0
    assert_goto_duration(profile, client, ra_w, -60.0, SPEED_DPS)
    assert client.get_value("sideofpier") == 0
    # correct to just east (HA=-1h) → pierWest (flip)
    ra_e = (lst + 1.0) % 24.0
    assert_goto_duration(profile, client, ra_e, -60.0, SPEED_DPS)
    assert client.get_value("sideofpier") == 1

