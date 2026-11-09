"""GUI — Autoguiding and fine corrections (ACTION guide)."""
import time

import pytest

from client import DeviceError

DIRS = {0: "north", 1: "south", 2: "east", 3: "west"}


@pytest.fixture(autouse=True)
def _quiet(proto):
    yield
    proto.action("stop")
    proto.control(tracking="none")


@pytest.mark.parametrize("direction", [0, 1, 2, 3])
def test_pulse_guide_directions(proto, direction):
    """ACTION guide accepts the four celestial directions."""
    proto.action("guide", direction=DIRS[direction], duration_ms=100)
    assert proto.state()["guiding"] in (True, False)


def test_pulse_duration_controls_correction(proto):
    """A pulse of valid duration ends (guiding becomes false)."""
    proto.action("guide", direction="north", duration_ms=300)
    time.sleep(0.5)
    assert proto.state()["guiding"] is False


def test_set_guide_rates(proto):
    """Change the guide rates via CONFIG and read them back."""
    proto.config_set(guide_rate_ra=0.001, guide_rate_dec=0.001)
    cfg = proto.config_get()["config"]
    assert cfg["guide_rate_ra"] > 0
    assert cfg["guide_rate_dec"] > 0


def test_guide_ra_and_dec(proto):
    """Guide simultaneously in RA and DEC (both axes accepted)."""
    proto.action("guide", direction="north", duration_ms=200)
    proto.action("guide", direction="east", duration_ms=200)


def test_guide_when_parked(proto):
    """ACTION guide while parked is rejected."""
    proto.action("park")
    with pytest.raises(DeviceError):
        proto.action("guide", direction="north", duration_ms=100)
    proto.action("unpark")


def test_guide_rate_negative_rejected(proto):
    """A negative guide rate is rejected and the stored rate is unchanged."""
    cfg = proto.config_get()["config"]
    with pytest.raises(DeviceError):
        proto.config_set(guide_rate_ra=-0.1)
    assert proto.config_get()["config"]["guide_rate_ra"] == cfg["guide_rate_ra"]


def test_guide_rate_above_ceiling_rejected(proto):
    """A guide rate above 1 deg/s is rejected (gateway contract is 0..1)."""
    with pytest.raises(DeviceError):
        proto.config_set(guide_rate_ra=1.5)


def test_guide_rate_zero_disables_axis_but_not_other(proto):
    """Zero is an explicit per-axis 'guiding disabled'; the other axis works."""
    proto.config_set(guide_rate_dec=0.0)
    try:
        with pytest.raises(DeviceError):
            proto.action("guide", direction="north", duration_ms=100)
        proto.action("guide", direction="east", duration_ms=100)
    finally:
        proto.config_set(guide_rate_dec=0.001)


def test_config_atomicity_on_invalid_guide_rate(proto):
    """A config with a valid site and an invalid guide rate changes neither."""
    cfg = proto.config_get()["config"]
    with pytest.raises(DeviceError):
        proto.config_set(lat=-33.0, guide_rate_ra=-0.5)
    after = proto.config_get()["config"]
    assert after["lat"] == cfg["lat"], "invalid guide_rate must not apply lat"
    assert after["guide_rate_ra"] == cfg["guide_rate_ra"]


def test_config_atomicity_on_invalid_utc(proto):
    """A config with a valid guide rate and an invalid UTC changes neither,
    nor does it bump the revision — the whole request is validated first."""
    before = proto.config_get()
    with pytest.raises(DeviceError):
        proto.config_set(guide_rate_dec=0.0, utc="invalid")
    after = proto.config_get()
    assert after["config"]["guide_rate_dec"] == before["config"]["guide_rate_dec"], \
        "invalid UTC must not apply guide_rate_dec (which would disable DEC)"
    assert after["config"]["lat"] == before["config"]["lat"], \
        "invalid UTC must not apply the site"
    assert after["config_rev"] == before["config_rev"], \
        "invalid UTC must not bump the config revision"


def test_config_wrong_type_rejected(proto):
    """A present field with the wrong JSON type is an error, not an absence."""
    cfg = proto.config_get()["config"]
    with pytest.raises(DeviceError):
        proto.config_set(lat="north")   # string where a number is required
    after = proto.config_get()["config"]
    assert after["lat"] == cfg["lat"], "a mistyped lat must not be silently ignored"
