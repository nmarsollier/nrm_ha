"""GOT-speed — the goto duration scales with the requested speed, and small
gotos (1-5°) take the model time.

Small-goto note: the firmware ramp (motors_motion_task.c) moves an axis at a
constant 0.8°/s (MIN_SLEW_CDS) when the move is under 2° (SHORT_SLEW_CDS) —
there is no acceleration curve there, it just crawls.  From 2° up to the
10° accel+decel window it ramps linearly (triangular profile).  These tests
verify the wall clock agrees with that model.
"""
import time

import pytest

from conftest import axis_position, goto_time, rest_post, wait_not_slewing
from oracle import MIN_SLEW_DPS

SPEEDS = [(1, 1.0), (2, 3.0), (3, 4.5), (4, 6.0)]


def _setup_site_time(client):
    client.put_ok("utcdate", form={"UTCDate": "2026-09-28T02:00:00"})
    client.put_ok("sitelatitude", form={"SiteLatitude": "-32.89"})
    client.put_ok("sitelongitude", form={"SiteLongitude": "-68.83"})


@pytest.fixture(autouse=True)
def _quiet(client):
    yield
    client.put_ok("abortslew")
    client.put_ok("tracking", form={"Tracking": "false"})


def _measure(profile, client, goto_fn):
    """Run goto_fn and return (elapsed_s, ra_dist_deg, dec_dist_deg)."""
    start = axis_position(profile)
    t0 = time.monotonic()
    goto_fn()
    wait_not_slewing(client)
    elapsed = time.monotonic() - t0
    end = axis_position(profile)
    return elapsed, abs(end["ra_deg"] - start["ra_deg"]), abs(end["dec_deg"] - start["dec_deg"])


def _check(elapsed, ra_dist, dec_dist, speed_dps, slack_s):
    expected = goto_time(ra_dist, dec_dist, speed_dps)
    assert elapsed >= expected * 0.6, \
        f"goto too fast: {elapsed:.2f}s < {expected * 0.6:.2f}s (model {expected:.2f}s)"
    assert elapsed <= expected * 1.4 + slack_s, \
        f"goto too slow: {elapsed:.2f}s > {expected * 1.4 + slack_s:.2f}s (model {expected:.2f}s)"


@pytest.mark.parametrize("speed_rate,speed_dps", SPEEDS)
def test_goto_speed_duration(profile, client, speed_rate, speed_dps):
    """The goto time matches the ramp model for the requested speed."""
    _setup_site_time(client)
    # establish a reference position (fast) so the timed goto is a small 5° move
    client.put_ok("slewtocoordinatesasync",
                  form={"RightAscension": "6.0", "Declination": "-55.0"})
    wait_not_slewing(client)

    elapsed, ra_dist, dec_dist = _measure(
        profile, client,
        lambda: rest_post(profile, "/api/slew-to-coordinates",
                          {"ra": 6.0, "dec": -60.0, "speed": speed_rate}))
    _check(elapsed, ra_dist, dec_dist, speed_dps, slack_s=3.0)


@pytest.mark.parametrize("delta", [1.0, 3.0, 5.0])
def test_small_goto_time(profile, client, delta):
    """A small goto of `delta` degrees (DEC) takes the model time.

    The firmware moves at a constant MIN_SLEW_DPS below SHORT_SLEW_CDS (2°) —
    no acceleration curve — and ramps above it.  (The exact 2° boundary is
    fuzzy: the step→cds truncation makes a nominal 2° move land at 199 cds,
    just under the threshold, so it still crawls.  We test 1° and 3°/5° to pin
    the two regimes without riding the boundary.)
    """
    _setup_site_time(client)
    lst = client.get_value("siderealtime")
    # west of the meridian (HA=+4h) so pier side is unambiguous; DEC=-60
    ra = (lst - 4.0) % 24.0
    client.put_ok("slewtocoordinatesasync",
                  form={"RightAscension": str(ra), "Declination": "-60.0"})
    wait_not_slewing(client)

    # a small DEC move (no RA change, same pier side)
    elapsed, ra_dist, dec_dist = _measure(
        profile, client,
        lambda: client.put_ok("slewtocoordinatesasync",
                              form={"RightAscension": str(ra),
                                    "Declination": str(-60.0 - delta)}))
    assert ra_dist < 0.5, f"small goto moved RA {ra_dist:.2f}° (expected ~0)"
    assert abs(dec_dist - delta) < 1.0, \
        f"small goto moved DEC {dec_dist:.2f}° (expected ~{delta}°)"

    # explicit boundary: below 2° the axis crawls at a constant MIN_SLEW_DPS
    # (no acceleration curve); above 2° it ramps, so it beats the crawl.
    const_time = dec_dist / MIN_SLEW_DPS
    if delta < 2.0:
        assert abs(elapsed - const_time) <= 0.75, \
            f"<2° goto should be constant {MIN_SLEW_DPS}°/s (~{const_time:.2f}s), got {elapsed:.2f}s"
    else:
        assert elapsed < const_time, \
            f">2° goto should ramp (faster than {const_time:.2f}s), got {elapsed:.2f}s"

    # tight slack: small moves have short, well-defined times
    _check(elapsed, ra_dist, dec_dist, 6.0, slack_s=1.0)
