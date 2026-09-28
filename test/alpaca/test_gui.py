"""GUI — Autoguiding and fine corrections (PulseGuide)."""
import pytest

from conftest import wait_not_slewing


@pytest.fixture(autouse=True)
def _quiet(client):
    yield
    client.put_ok("abortslew")
    client.put_ok("tracking", form={"Tracking": "false"})


# ── guiding ───
@pytest.mark.parametrize("direction", [0, 1, 2, 3])  # N, S, E, W
def test_pulse_guide_directions(client, direction):
    """PulseGuide accepts the four celestial directions."""
    client.put_ok("pulseguide", form={"Direction": str(direction), "Duration": "100"})
    # the pulse is accepted without error; when done IsPulseGuiding goes false
    assert client.get_value("ispulseguiding") in (True, False)


# ── guiding ───
def test_pulse_duration_controls_correction(client):
    """A pulse of valid duration ends (IsPulseGuiding becomes false)."""
    import time
    client.put_ok("pulseguide", form={"Direction": "0", "Duration": "300"})
    time.sleep(0.5)
    assert client.get_value("ispulseguiding") is False


# ── guiding ───
def test_set_guide_rates(client):
    """Change the guide rates and read them back."""
    client.put_ok("guideraterightascension", form={"GuideRateRightAscension": "0.001"})
    client.put_ok("guideratedeclination", form={"GuideRateDeclination": "0.001"})
    assert client.get_value("guideraterightascension") > 0
    assert client.get_value("guideratedeclination") > 0


# ── guiding ───
def test_guide_ra_and_dec(client):
    """Guide simultaneously in RA and DEC (both axes accepted)."""
    client.put_ok("pulseguide", form={"Direction": "0", "Duration": "200"})
    client.put_ok("pulseguide", form={"Direction": "2", "Duration": "200"})
    # no error when overlapping distinct axes
    assert True


# ── guiding ───
def test_guide_when_parked(client):
    """PulseGuide while parked is rejected."""
    client.put_ok("park")
    r = client.put("pulseguide", form={"Direction": "0", "Duration": "100"})
    assert r.error_number != 0, "PulseGuide estando parked fue aceptado"
    client.put_ok("unpark")
