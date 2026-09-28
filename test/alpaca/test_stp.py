"""STP — Stop and cancellation."""
import time

import pytest

from conftest import wait_not_slewing, wait_slewing, wait_tracking, rest_post


def _set_time(client):
    """Set a valid UTC time to enable GOTO (mount requirement)."""
    client.put_ok("utcdate", form={"UTCDate": "2026-09-28T02:00:00"})
    client.put_ok("sitelatitude", form={"SiteLatitude": "-32.89"})
    client.put_ok("sitelongitude", form={"SiteLongitude": "-68.83"})


@pytest.fixture(autouse=True)
def _quiet(client):
    yield
    client.put_ok("abortslew")
    client.put_ok("tracking", form={"Tracking": "false"})


# ── stop ───
def test_abort_slew(client):
    """AbortSlew stops an in-progress maneuver (Slewing becomes false)."""
    _set_time(client)
    client.put_ok("tracking", form={"Tracking": "false"})
    # distant target so the maneuver lasts a few seconds
    client.put_ok("slewtocoordinatesasync",
                  form={"RightAscension": "6.0", "Declination": "-60.0"})
    wait_slewing(client)
    client.put_ok("abortslew")
    wait_not_slewing(client)
    assert client.get_value("slewing") is False


# ── stop ───
def test_stop_cancels_queued_goto(client):
    """A goto queued behind an active one is cancelled by STOP and never runs."""
    _set_time(client)
    client.put_ok("slewtocoordinatesasync",
                  form={"RightAscension": "6.0", "Declination": "-60.0"})
    wait_slewing(client)

    # queue a second goto behind the active one (accepted, not executed yet)
    client.put_ok("slewtocoordinatesasync",
                  form={"RightAscension": "7.0", "Declination": "-50.0"})

    client.put_ok("abortslew")
    wait_not_slewing(client)

    # ample time for the cancelled goto to (wrongly) start; it must not
    time.sleep(5.0)
    assert client.get_value("slewing") is False, "cancelled goto re-appeared after stop"


# ── stop ───
def test_repeat_stop_when_quiet(client):
    """Repeating stop while quiet is safe and idempotent."""
    client.put_ok("abortslew")
    for _ in range(3):
        r = client.put("abortslew")
        assert r.error_number == 0, f"AbortSlew while quiet returned {r.error_number}"
    assert client.get_value("slewing") is False


# ── stop ───
def test_move_again_after_stop(client):
    """After a stop the mount can move again."""
    _set_time(client)
    client.put_ok("slewtocoordinatesasync",
                  form={"RightAscension": "6.0", "Declination": "-60.0"})
    wait_slewing(client)
    client.put_ok("abortslew")
    wait_not_slewing(client)

    # a new move is accepted
    client.put_ok("slewtocoordinatesasync",
                  form={"RightAscension": "6.0", "Declination": "-60.0"})
    wait_slewing(client)
    client.put_ok("abortslew")
    wait_not_slewing(client)


# ── parada — (REST global) ───
def test_global_stop_rest(profile, client):
    """Global STOP via REST halts the maneuver (REST complementary)."""
    import json
    import urllib.request
    _set_time(client)
    client.put_ok("slewtocoordinatesasync",
                  form={"RightAscension": "6.0", "Declination": "-60.0"})
    wait_slewing(client)

    req = urllib.request.Request(profile["rest_base"] + "/api/stop", data=b"{}",
                                 method="POST", headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=5) as r:
        body = json.load(r)
    assert body.get("ok") is True
    wait_not_slewing(client)


# ── parada — tracking ───
def test_global_stop_clears_tracking(profile, client):
    """The global REST STOP clears an active tracking."""
    _set_time(client)
    client.put_ok("tracking", form={"Tracking": "true"})
    wait_tracking(client, True)

    rest_post(profile, "/api/stop", {})
    wait_tracking(client, False)
    assert client.get_value("tracking") is False


def test_abort_slew_preserves_tracking(client):
    """AbortSlew during tracking preserves the tracking mode."""
    _set_time(client)
    client.put_ok("tracking", form={"Tracking": "true"})
    wait_tracking(client, True)

    client.put_ok("abortslew")
    assert client.get_value("tracking") is True, "AbortSlew cleared tracking"


def test_stop_then_park(client):
    """Stop followed by park leaves the mount parked."""
    _set_time(client)
    client.put_ok("tracking", form={"Tracking": "true"})
    wait_tracking(client, True)

    client.put_ok("abortslew")
    client.put_ok("park")
    assert client.get_value("atpark") is True
    client.put_ok("unpark")
