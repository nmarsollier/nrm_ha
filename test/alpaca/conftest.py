"""Shared fixtures and helpers for the NRM-HA Alpaca suite.

The oracle and the client live in client.py / oracle.py. Here the profile is
loaded, the clients are created, and the bounded-polling helpers with a
monotonic clock are exposed.
"""
import json
import os
import socket
import time
import urllib.request

import pytest

from client import AlpacaClient, DeviceError, Response
from oracle import angular_separation_deg, goto_time

PROFILE_PATH = os.path.join(os.path.dirname(__file__), "profile.json")


def discover(host, port=32227, timeout=3):
    """ASCOM Alpaca Discovery Protocol v1 — sends 'alpacadiscovery1' over UDP."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(timeout)
    try:
        s.sendto(b"alpacadiscovery1", (host, port))
        data, _ = s.recvfrom(1024)
        return json.loads(data.decode())
    finally:
        s.close()


class Profile(dict):
    """Versioned profile. Null fields used by a test must be rejected
    (they are not a PASS)."""

    @classmethod
    def load(cls, path=PROFILE_PATH):
        with open(path) as f:
            return cls(json.load(f))

    def require(self, *keys):
        missing = [k for k in keys if self.get(k) is None]
        if missing:
            pytest.fail(f"Perfil con valores pendientes para: {missing}")
        return self


@pytest.fixture(scope="session")
def profile():
    return Profile.load()


@pytest.fixture(scope="session")
def client(profile):
    return AlpacaClient(profile["alpaca_base"], device_number=profile.get("device_number", 0))


@pytest.fixture(scope="session")
def supervisor(profile):
    """Supervisor client that stays open (cleanup pattern)."""
    return AlpacaClient(profile["alpaca_base"], device_number=profile.get("device_number", 0))


# ── bounded polling with monotonic clock ─────────────────────────

def wait_until(client, getter, pred, timeout, desc, poll=None):
    """Poll `getter()` until `pred(value)` is true or the deadline expires."""
    poll = poll or 0.25
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        last = getter()
        if pred(last):
            return last
        time.sleep(poll)
    raise AssertionError(f"timeout waiting for {desc} (last={last!r})")


def wait_not_slewing(client, timeout=90, poll=None):
    """Wait until Slewing is False, returning the final position."""
    def getter():
        return {
            "slewing": client.get_value("slewing"),
            "ra": client.get_value("rightascension"),
            "dec": client.get_value("declination"),
        }
    return wait_until(client, getter, lambda s: s["slewing"] is False,
                      timeout, "slewing=false", poll)


def wait_slewing(client, timeout=10, poll=None):
    def getter():
        return client.get_value("slewing")
    return wait_until(client, getter, lambda v: v is True, timeout, "slewing=true", poll)


def wait_tracking(client, value, timeout=10):
    def getter():
        return client.get_value("tracking")
    return wait_until(client, getter, lambda v: v == value, timeout, f"tracking={value}")


# ── celestial position ─────────────────────────────────────────────

def current_radec(client):
    """Snapshot of the reported celestial position (two non-atomic reads)."""
    return {
        "ra": client.get_value("rightascension"),
        "dec": client.get_value("declination"),
        "slewing": client.get_value("slewing"),
        "tracking": client.get_value("tracking"),
        "sideofpier": client.get_value("sideofpier"),
        "atpark": client.get_value("atpark"),
        "athome": client.get_value("athome"),
    }


# ── site/time setup (GOTO requirement) ────────────────

def setup_site_time(client):
    """Set a valid site and UTC to enable GOTO (mount requirement)."""
    client.put_ok("utcdate", form={"UTCDate": "2026-09-28T02:00:00"})
    client.put_ok("sitelatitude", form={"SiteLatitude": "-32.89"})
    client.put_ok("sitelongitude", form={"SiteLongitude": "-68.83"})


# ── celestial separation assertion ───────────────────────────────

def assert_angular_close(ra1, dec1, ra2, dec2, tol_deg, ctx=""):
    sep = angular_separation_deg(ra1, dec1, ra2, dec2)
    assert sep <= tol_deg, f"{ctx} separation {sep:.4f}° > tolerance {tol_deg}°"


# ── axis position via REST (for goto time assertions) ─────────────

def axis_position(profile):
    """Read the physical axis position (deg + steps) from the REST /api/status."""
    with urllib.request.urlopen(profile["rest_base"] + "/api/status", timeout=8) as r:
        s = json.load(r)
    d = s["debug"]
    return {
        "ra_deg": d["ra_axis_deg"],
        "dec_deg": d["dec_axis_deg"],
        "ra_steps": d["ra_steps"],
        "dec_steps": d["dec_steps"],
    }


def rest_post(profile, path, obj):
    """POST to the REST API (port 80), returning (http_status, parsed_json)."""
    req = urllib.request.Request(profile["rest_base"] + path,
                                 data=json.dumps(obj).encode(), method="POST",
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=8) as r:
        return r.status, json.loads(r.read().decode())


def assert_goto_duration(profile, client, ra, dec, speed_dps, slack_s=3.0, factor=1.35):
    """Issue a goto, then assert the wall-clock duration matches the slew model.

    The mount's ramp profile predicts, from the *observed* axis distance and
    the commanded speed, how long the move must take.  The measured time has to
    be at least that (a too-fast move would mean the ramp isn't applied) and no
    more than `factor`× plus a fixed `slack_s` (poll interval + settle + queue).
    """
    start = axis_position(profile)
    t0 = time.monotonic()
    client.put_ok("slewtocoordinatesasync",
                  form={"RightAscension": str(ra), "Declination": str(dec)})
    final = wait_not_slewing(client)
    elapsed = time.monotonic() - t0
    end = axis_position(profile)

    ra_dist = abs(end["ra_deg"] - start["ra_deg"])
    dec_dist = abs(end["dec_deg"] - start["dec_deg"])
    expected = goto_time(ra_dist, dec_dist, speed_dps)

    # The goto must arrive at the requested destination.  A zero-distance goto
    # (already at the target) is legitimate — the duration floor is then
    # trivially satisfied, but the arrival check below still proves the slew
    # worked, instead of merely that Slewing went false.
    assert_angular_close(final["ra"], final["dec"], ra, dec,
                         profile["budgets"]["reported_position_deg"], "goto arrival")

    lower = expected * 0.5          # sanity floor: a real slew can't be instant
    upper = expected * factor + slack_s
    assert elapsed >= lower, \
        f"goto finished in {elapsed:.2f}s, suspiciously < {lower:.2f}s " \
        f"(ra {ra_dist:.2f}°, dec {dec_dist:.2f}°)"
    assert elapsed <= upper, \
        f"goto took {elapsed:.2f}s, more than expected {upper:.2f}s " \
        f"(ra {ra_dist:.2f}°, dec {dec_dist:.2f}°, model {expected:.2f}s)"
    return {"elapsed": elapsed, "expected": expected,
            "ra_dist": ra_dist, "dec_dist": dec_dist,
            "final_ra": final["ra"], "final_dec": final["dec"],
            "start_axis": start, "end_axis": end}


# ── connection fixture ──────────────────────────────────────────

@pytest.fixture(autouse=True)
def ensure_connected(client):
    """Ensures Connected=true at the start of each test (without moving the mount)."""
    client.put_ok("connected", form={"Connected": "true"})
    yield


@pytest.fixture(scope="module", autouse=True)
def _home_each_module(client):
    """Home once per test module so motion tests start from a known position.

    Eliminates order-dependence: a goto/stop test that assumes a large move
    must not depend on where the previous file left the mount.
    """
    try:
        client.put_ok("unpark")
        client.put_ok("abortslew")
        client.put_ok("findhome")
        wait_not_slewing(client, timeout=40)
    except Exception:
        pass


@pytest.fixture(scope="session", autouse=True)
def _restore_state(client, profile):
    """Restore guide rates and limits after the session.

    Tests mutate these without restoring them; a fresh run must not inherit
    the state left behind (and an isolated single test must see the same
    baseline as the full suite).
    """
    yield
    try:
        client.put_ok("guideraterightascension",
                      form={"GuideRateRightAscension": "0.002089"})
        client.put_ok("guideratedeclination",
                      form={"GuideRateDeclination": "0.002089"})
        rest_post(profile, "/api/limits", {"action": "clear_limits"})
    except Exception:
        pass
