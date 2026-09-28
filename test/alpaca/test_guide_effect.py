"""GUIDE-effect — a PulseGuide moves the axis by the expected amount and time.

The pulse offset is the guide rate (deg/s) applied for the pulse duration, so
the axis position change is rate × duration.  The X-NRM-Snapshot header on the
pulse response carries the position at pulse receipt; the position after the
pulse is read back and the two are compared.
"""
import time

import pytest

from conftest import axis_position, wait_not_slewing

GUIDE_RATE = 0.5          # deg/s — high enough for a measurable effect
DEG_PER_STEP = 1.8 / (64 * 300)


def _setup_site_time(client):
    client.put_ok("utcdate", form={"UTCDate": "2026-09-28T02:00:00"})
    client.put_ok("sitelatitude", form={"SiteLatitude": "-32.89"})
    client.put_ok("sitelongitude", form={"SiteLongitude": "-68.83"})


def _set_guide_rate(client, rate):
    client.put_ok("guideraterightascension", form={"GuideRateRightAscension": str(rate)})
    client.put_ok("guideratedeclination", form={"GuideRateDeclination": str(rate)})


@pytest.fixture(autouse=True)
def _quiet(client):
    yield
    client.put_ok("abortslew")
    client.put_ok("tracking", form={"Tracking": "false"})


def _measure_pulse(profile, client, direction, duration_ms):
    """Issue a pulse; return (before_snapshot, after_axis, elapsed_s).

    The pulse is async: wait for ispulseguiding to go True then False (now
    observable over HTTP), then read the post-pulse position.  elapsed is the
    time from the response to the pulse end, ≈ the pulse duration.
    """
    r = client.put("pulseguide", form={"Direction": str(direction), "Duration": str(duration_ms)})
    assert r.error_number == 0, f"pulseguide rejected: {r.error_number}"
    before = r.snapshot()
    assert before is not None, "no X-NRM-Snapshot header on pulseguide"

    t0 = time.monotonic()
    deadline = t0 + duration_ms / 1000.0 + 5.0
    saw_guiding = False
    while time.monotonic() < deadline:
        if client.get_value("ispulseguiding"):
            saw_guiding = True
        elif saw_guiding:
            break
        time.sleep(0.02)
    elapsed = time.monotonic() - t0
    after = axis_position(profile)
    return before, after, elapsed


def _assert_dec_change(before, after, duration_ms, expected_sign=None):
    """Check the DEC axis moved by rate × duration (sign per pier side)."""
    change_steps = after["dec_steps"] - before["dec"]
    expected_steps = GUIDE_RATE * duration_ms / 1000.0 / DEG_PER_STEP
    assert abs(abs(change_steps) - expected_steps) <= expected_steps * 0.3 + 4, \
        f"DEC change {change_steps} steps != expected ~{expected_steps:.0f}"
    if expected_sign is not None:
        assert (change_steps > 0) == (expected_sign > 0), \
            f"DEC change sign wrong: {change_steps} (expected {expected_sign})"


def test_pulse_effect_magnitude_and_time(profile, client):
    """A DEC pulse moves the axis by rate × duration, and takes ~duration."""
    _set_guide_rate(client, GUIDE_RATE)
    before, after, elapsed = _measure_pulse(profile, client, 0, 500)  # North
    _assert_dec_change(before, after, 500)
    # the pulse lasts ~500 ms (plus a small command/queue overhead)
    assert 0.4 <= elapsed <= 2.0, f"pulse took {elapsed:.2f}s (expected ~0.5s)"


def test_pulse_east_moves_ra_negative(profile, client):
    """An East RA pulse moves the mechanical RA axis negative (East = -RA)."""
    _setup_site_time(client)
    _set_guide_rate(client, GUIDE_RATE)
    before, after, _ = _measure_pulse(profile, client, 2, 500)  # East
    assert after["ra_steps"] < before["ra"], \
        f"East pulse did not move RA negative ({before['ra']} -> {after['ra_steps']})"


def test_pulse_flip_reverses_dec(profile, client):
    """A North pulse moves DEC + on pierEast and − on pierWest (flip)."""
    _setup_site_time(client)
    _set_guide_rate(client, GUIDE_RATE)
    lst = client.get_value("siderealtime")

    # west of the meridian → pierEast (dec_axis > 0)
    ra_west = (lst - 4.0) % 24.0
    client.put_ok("slewtocoordinatesasync",
                  form={"RightAscension": str(ra_west), "Declination": "-60.0"})
    wait_not_slewing(client)
    assert client.get_value("sideofpier") == 0
    b1, a1, _ = _measure_pulse(profile, client, 0, 500)  # North
    assert a1["dec_steps"] > b1["dec"], "North should increase dec_axis on pierEast"

    # east of the meridian → pierWest (dec_axis < 0)
    ra_east = (lst + 4.0) % 24.0
    client.put_ok("slewtocoordinatesasync",
                  form={"RightAscension": str(ra_east), "Declination": "-60.0"})
    wait_not_slewing(client)
    assert client.get_value("sideofpier") == 1
    b2, a2, _ = _measure_pulse(profile, client, 0, 500)  # North
    assert a2["dec_steps"] < b2["dec"], "North should decrease dec_axis on pierWest"


@pytest.mark.parametrize("tracking,rate", [(False, None), (True, 0), (True, 1), (True, 2)])
def test_pulse_tracking_modes(profile, client, tracking, rate):
    """A DEC pulse works with no tracking and with every tracking rate."""
    _setup_site_time(client)
    _set_guide_rate(client, GUIDE_RATE)
    if tracking:
        client.put_ok("tracking", form={"Tracking": "true"})
        client.put_ok("trackingrate", form={"TrackingRate": str(rate)})
    else:
        client.put_ok("tracking", form={"Tracking": "false"})
    before, after, _ = _measure_pulse(profile, client, 0, 500)  # North (DEC)
    _assert_dec_change(before, after, 500)


def test_pulse_at_dec_limit_clamped(profile, client):
    """A pulse toward the DEC axis limit is clamped (does not move past it)."""
    _setup_site_time(client)
    _set_guide_rate(client, GUIDE_RATE)
    lst = client.get_value("siderealtime")
    # DEC=+60 → dec_axis = ±150 (the DEC limit), at HA=0 on the meridian
    client.put_ok("slewtocoordinatesasync",
                  form={"RightAscension": str(lst), "Declination": "60.0"})
    wait_not_slewing(client)

    before, after, _ = _measure_pulse(profile, client, 0, 500)  # North → toward limit
    assert abs(after["dec_deg"]) <= 150.0 + 0.01, \
        f"DEC axis exceeded the limit: {after['dec_deg']}"
    # clamped: far less than the full ~2667-step pulse
    full_steps = GUIDE_RATE * 0.5 / DEG_PER_STEP
    assert abs(after["dec_steps"] - before["dec"]) < full_steps * 0.5, \
        f"pulse at limit moved DEC {after['dec_steps'] - before['dec']} steps (not clamped)"
