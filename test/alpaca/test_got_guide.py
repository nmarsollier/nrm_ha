"""GOT-guide — a PulseGuide issued mid-goto must not corrupt the slew."""
import pytest

from conftest import wait_not_slewing, wait_slewing


def _setup_site_time(client):
    client.put_ok("utcdate", form={"UTCDate": "2026-09-28T02:00:00"})
    client.put_ok("sitelatitude", form={"SiteLatitude": "-32.89"})
    client.put_ok("sitelongitude", form={"SiteLongitude": "-68.83"})


@pytest.fixture(autouse=True)
def _quiet(client):
    yield
    client.put_ok("abortslew")
    client.put_ok("tracking", form={"Tracking": "false"})


def test_pulse_guide_during_goto(client):
    """A PulseGuide issued mid-slew is deferred/absorbed without breaking the goto."""
    _setup_site_time(client)
    # home first so the following goto is always a large, in-flight move
    client.put_ok("findhome")
    wait_not_slewing(client)

    client.put_ok("slewtocoordinatesasync",
                  form={"RightAscension": "6.0", "Declination": "-60.0"})
    wait_slewing(client)

    # pulse guide while the slew is in flight
    r = client.put("pulseguide", form={"Direction": "0", "Duration": "200"})

    # the goto still completes and the mount ends coherent (not slewing)
    final = wait_not_slewing(client)
    assert final["slewing"] is False, "goto did not finish after pulse guide"
    # still responsive and not stuck guiding
    assert client.get_value("ispulseguiding") is False, "left pulse-guiding active"
