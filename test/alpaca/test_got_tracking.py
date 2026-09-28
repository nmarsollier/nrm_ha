"""GOT-tracking — a goto preserves (or leaves off) the tracking state."""
import pytest

from conftest import assert_goto_duration, wait_tracking

SPEED_DPS = 6.0


def _setup_site_time(client):
    client.put_ok("utcdate", form={"UTCDate": "2026-09-28T02:00:00"})
    client.put_ok("sitelatitude", form={"SiteLatitude": "-32.89"})
    client.put_ok("sitelongitude", form={"SiteLongitude": "-68.83"})


@pytest.fixture(autouse=True)
def _no_tracking(client):
    yield
    client.put_ok("tracking", form={"Tracking": "false"})


def test_goto_preserves_tracking(profile, client):
    """A goto issued while tracking is active resumes tracking afterwards."""
    _setup_site_time(client)
    client.put_ok("tracking", form={"Tracking": "true"})
    wait_tracking(client, True)
    assert_goto_duration(profile, client, 6.0, -60.0, SPEED_DPS)
    assert client.get_value("tracking") is True, "goto did not preserve tracking"


def test_goto_without_tracking_stays_off(profile, client):
    """A goto with tracking off leaves tracking off."""
    _setup_site_time(client)
    assert_goto_duration(profile, client, 6.0, -60.0, SPEED_DPS)
    assert client.get_value("tracking") is False


@pytest.mark.parametrize("rate", [0, 1, 2])  # sidereal, lunar, solar
def test_goto_preserves_tracking_rate(profile, client, rate):
    """A goto preserves both tracking state and the selected rate."""
    _setup_site_time(client)
    client.put_ok("tracking", form={"Tracking": "true"})
    client.put_ok("trackingrate", form={"TrackingRate": str(rate)})
    wait_tracking(client, True)
    assert_goto_duration(profile, client, 6.0, -60.0, SPEED_DPS)
    assert client.get_value("tracking") is True, "goto did not preserve tracking"
    assert client.get_value("trackingrate") == rate, \
        f"goto changed rate from {rate} to {client.get_value('trackingrate')}"
