"""GOT-time — a goto's duration must match the slew ramp model.

The firmware accelerates each axis along a fixed distance-based ramp (min
0.8°/s up to the commanded speed, capped at 3.2°/s for gentle moves).  Given
the axis distance actually travelled and the commanded speed, the model in
oracle.py predicts the move time; these tests assert the wall clock agrees.
"""
import pytest

from conftest import assert_goto_duration

SPEED_DPS = 6.0  # Alpaca goto uses speed_rate 4 → 6°/s


def _setup_site_time(client):
    client.put_ok("utcdate", form={"UTCDate": "2026-09-28T02:00:00"})
    client.put_ok("sitelatitude", form={"SiteLatitude": "-32.89"})
    client.put_ok("sitelongitude", form={"SiteLongitude": "-68.83"})


@pytest.fixture(autouse=True)
def _quiet(client):
    yield
    client.put_ok("abortslew")
    client.put_ok("tracking", form={"Tracking": "false"})


def test_goto_duration_matches_ramp(profile, client):
    """A goto takes the time the acceleration ramp dictates."""
    _setup_site_time(client)
    assert_goto_duration(profile, client, 6.0, -60.0, SPEED_DPS)


def test_short_goto_duration(profile, client):
    """A short goto (triangular ramp) still matches the model."""
    _setup_site_time(client)
    assert_goto_duration(profile, client, 6.0, -65.0, SPEED_DPS)
