"""SESSION — long mixed sessions and interior-limit behaviour.

These cover what a real N.I.N.A. + guiding night looks like: tracking with
interleaved queries, small gotos, guide pulses, cancellation and a final park;
and a target that becomes unreachable because the mount runs into a test limit.
"""
import time

import pytest

from conftest import (axis_position, rest_post, setup_site_time, wait_not_slewing,
                      wait_tracking)
from client import AlpacaClient


@pytest.fixture(autouse=True)
def _clear_limits_after(profile):
    """Tests here set interior limits; restore the factory limits afterwards."""
    yield
    try:
        rest_post(profile, "/api/limits", {"action": "clear_limits"})
    except Exception:
        pass


def test_tracking_stops_at_test_limit(profile, client):
    """Tracking toward an interior test limit stops there, without passing it."""
    setup_site_time(client)

    # home, then move RA positive and set the right limit at that position
    client.put_ok("findhome")
    wait_not_slewing(client)
    client.put_ok("moveaxis", form={"Axis": "0", "Rate": "2.0"})
    time.sleep(3.0)
    client.put_ok("moveaxis", form={"Axis": "0", "Rate": "0.0"})
    wait_not_slewing(client)
    rest_post(profile, "/api/limits", {"action": "set_ra_right"})
    limit = axis_position(profile)["ra_deg"]

    # move back toward the interior, then track toward the limit
    client.put_ok("moveaxis", form={"Axis": "0", "Rate": "-2.0"})
    time.sleep(3.0)
    client.put_ok("moveaxis", form={"Axis": "0", "Rate": "0.0"})
    wait_not_slewing(client)

    client.put_ok("tracking", form={"Tracking": "true"})
    time.sleep(3.0)
    pos = axis_position(profile)
    assert pos["ra_deg"] <= limit + 0.1, \
        f"tracking passed the test limit {limit:.2f}° (now {pos['ra_deg']:.2f}°)"

    # recovery: stop tracking and move back toward the interior
    client.put_ok("tracking", form={"Tracking": "false"})
    client.put_ok("moveaxis", form={"Axis": "0", "Rate": "-2.0"})
    time.sleep(1.0)
    client.put_ok("moveaxis", form={"Axis": "0", "Rate": "0.0"})
    wait_not_slewing(client)


def test_prolonged_mixed_session(profile, client):
    """Tracking + queries + guide + small goto + cancel + park, staying coherent."""
    setup_site_time(client)
    other = AlpacaClient(profile["alpaca_base"], device_number=0)

    client.put_ok("tracking", form={"Tracking": "true"})
    wait_tracking(client, True)

    # a guide pulse while tracking
    client.put_ok("guideratedeclination", form={"GuideRateDeclination": "0.5"})
    client.put_ok("pulseguide", form={"Direction": "0", "Duration": "300"})

    # a small goto preserves tracking
    client.put_ok("slewtocoordinatesasync",
                  form={"RightAscension": "6.0", "Declination": "-60.0"})
    wait_not_slewing(client)
    assert client.get_value("tracking") is True, "tracking lost after goto"

    # a second client observes without disturbing anything
    assert other.get_value("slewing") is False
    assert other.get_value("tracking") is True

    # cancel and park cleanly
    client.put_ok("abortslew")
    client.put_ok("park")
    assert client.get_value("atpark") is True
    client.put_ok("unpark")
    assert client.get_value("atpark") is False

    # return to a known state so later tests start from home, not the goto target
    client.put_ok("findhome")
    wait_not_slewing(client)
