"""TRK — Tracking during observation and exposure."""
import time

import pytest

from conftest import wait_tracking, wait_not_slewing, axis_position

# Alpaca tracking rate mapping: 0=sidereal, 1=lunar, 2=solar (deg/s)
RATES = {0: 0.004178074, 1: 0.004025576, 2: 0.004166667}


def _ra_wrap_diff(a, b):
    """Wrapped |a - b| in hours, handling the 0/24 h right-ascension boundary."""
    d = abs(a - b)
    return min(d, 24.0 - d)


@pytest.fixture(autouse=True)
def _no_tracking(client):
    """Leaves tracking off at the end of each test."""
    yield
    client.put_ok("tracking", form={"Tracking": "false"})


# ── tracking ───
def test_disable_tracking(client):
    """Tracking=false disables tracking."""
    client.put_ok("tracking", form={"Tracking": "true"})
    wait_tracking(client, True)
    client.put_ok("tracking", form={"Tracking": "false"})
    wait_tracking(client, False)
    assert client.get_value("tracking") is False


# ── tracking ───
def test_reactivate_tracking(client):
    """Reactivating after a pause keeps the selection."""
    client.put_ok("tracking", form={"Tracking": "true"})
    wait_tracking(client, True)
    client.put_ok("tracking", form={"Tracking": "false"})
    wait_tracking(client, False)
    client.put_ok("tracking", form={"Tracking": "true"})
    wait_tracking(client, True)
    assert client.get_value("tracking") is True


# ── tracking ───
@pytest.mark.parametrize("rate", [0, 1, 2])
def test_change_tracking_rate(client, rate):
    """Switch between sidereal (0), lunar (1) and solar (2)."""
    client.put_ok("trackingrate", form={"TrackingRate": str(rate)})
    assert client.get_value("trackingrate") == rate


# ── tracking ───
def test_tracking_order_idempotent(client):
    """Repeating the same order accumulates no errors."""
    for _ in range(5):
        client.put_ok("tracking", form={"Tracking": "true"})
        assert client.get_value("tracking") is True
    client.put_ok("tracking", form={"Tracking": "false"})
    wait_tracking(client, False)


# ── tracking ───
def test_tracking_keeps_ra_stable(client):
    """Tracking compensates sidereal drift: RA stays put, unlike at rest.

    The sky drifts ~0.000836 h of RA in 3 s at the sidereal rate.  Measured
    against that baseline (tracking OFF), tracking ON must reduce the drift to
    a fraction — otherwise the mount isn't actually following the sky.
    """
    # Baseline: drift with tracking OFF (mount at rest, the sky moves).
    client.put_ok("tracking", form={"Tracking": "false"})
    wait_tracking(client, False)
    ra0 = client.get_value("rightascension")
    time.sleep(3.0)
    ra1 = client.get_value("rightascension")
    drift_off = _ra_wrap_diff(ra0, ra1)

    # Drift with tracking ON (mount follows the sky).
    client.put_ok("tracking", form={"Tracking": "true"})
    wait_tracking(client, True)
    ra2 = client.get_value("rightascension")
    time.sleep(3.0)
    ra3 = client.get_value("rightascension")
    drift_on = _ra_wrap_diff(ra2, ra3)

    # The baseline must be measurable (~0.000836 h), otherwise the measurement
    # itself is suspect (wrong time/site, or the sky isn't drifting).
    assert drift_off > 0.0003, f"baseline drift too small to measure: {drift_off:.6f} h"
    assert drift_on < drift_off * 0.25, \
        f"tracking did not hold RA: off={drift_off:.6f} h on={drift_on:.6f} h"


# ── tracking ───
def test_tracking_prolonged_with_queries(client):
    """Sustained tracking with interleaved queries does not degrade state.

    (Bounded version of the long campaign: 10 s with polls. RA stability is
    already validated in test_tracking_keeps_ra_stable.)
    """
    client.put_ok("tracking", form={"Tracking": "true"})
    for _ in range(10):
        assert client.get_value("tracking") is True
        assert client.get_value("slewing") is False
        time.sleep(0.5)
    client.put_ok("tracking", form={"Tracking": "false"})


@pytest.mark.parametrize("rate", [0, 1, 2])
def test_tracking_rate_coherence(profile, client, rate):
    """Tracking moves the RA axis at the mode's rate (deg/s)."""
    client.put_ok("tracking", form={"Tracking": "true"})
    client.put_ok("trackingrate", form={"TrackingRate": str(rate)})
    wait_tracking(client, True)

    t0 = axis_position(profile)
    time.sleep(15.0)
    t1 = axis_position(profile)

    ra_change = t1["ra_deg"] - t0["ra_deg"]
    expected = RATES[rate] * 15.0
    # Tight (±3%) so lunar (3.65% below sidereal) can't pass while the mount
    # tracks at the wrong rate.  Solar is only 0.27% from sidereal, so it is
    # not distinguishable in this window — a physical limit, not a gap.
    assert expected * 0.97 <= ra_change <= expected * 1.03, \
        f"rate {rate}: RA moved {ra_change:.6f}° in 15s, expected ~{expected:.6f}°"


def test_tracking_stops_at_limit(profile, client):
    """Tracking stops when the RA axis reaches its limit (does not pass it)."""
    client.put_ok("findhome")
    wait_not_slewing(client)

    # move RA to the positive limit (+100°)
    client.put_ok("moveaxis", form={"Axis": "0", "Rate": "6.0"})
    wait_not_slewing(client, timeout=40)
    pos = axis_position(profile)
    assert pos["ra_deg"] > 99.0, f"RA not near the limit: {pos['ra_deg']}"

    # tracking pushes RA positive; at the limit it must stop, not pass
    client.put_ok("tracking", form={"Tracking": "true"})
    time.sleep(2.0)
    assert client.get_value("tracking") is False, "tracking did not stop at the limit"
    pos2 = axis_position(profile)
    assert pos2["ra_deg"] <= 100.0 + 0.1, f"RA passed the limit: {pos2['ra_deg']}"

