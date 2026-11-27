"""GOT-sites — the same goto resolves correctly from different site coordinates."""
import pytest

from conftest import assert_goto_duration


@pytest.fixture(autouse=True)
def _quiet(proto):
    yield
    proto.action("stop")
    proto.control(tracking="none")


@pytest.mark.parametrize("lat,lon", [
    (-32.89, -68.83),   # southern, home site
    (-23.55, -46.63),   # southern, different longitude
    (40.71, -74.01),    # northern
])
def test_goto_across_sites(proto, caps, lat, lon):
    proto.config_set(lat=lat, lon=lon, elevation=750,
                     utc="2026-09-28T02:00:00Z")
    # DEC=-30 is reachable from both hemispheres (the fork maps the reference
    # pole differently north vs south, so a southern target like -60° would be
    # out of the DEC envelope at the northern site).
    assert_goto_duration(proto, caps, 6.0, -30.0)
