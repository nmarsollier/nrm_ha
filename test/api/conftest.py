"""Shared helpers + fixtures for the NRM-HA black-box state-machine suite.

Every test observes state ONLY through the public REST + Alpaca surface, and
moves are bounded by BENCH_ENVELOPE_DEG.  A session fixture returns the mount
to HOME at teardown.
"""
import json
import os
import time
import urllib.error
import urllib.parse
import urllib.request

import pytest

HOST = os.environ.get("NRM_HOST", "192.168.7.1")
REST = f"http://{HOST}"
ALPACA = f"http://{HOST}:11111"
T = "/api/v1/telescope/0"

# Safety envelope: reject any single commanded move that would take an axis
# farther than this many degrees from home (0,0).  Bench-dependent.
BENCH_ENVELOPE_DEG = float(os.environ.get("NRM_ENVELOPE_DEG", "12"))


class MountError(RuntimeError):
    pass


def http(method, url, body=None, timeout=8, headers=None):
    data = None if body is None else body.encode()
    hdrs = dict(headers or {})
    if body is not None:
        hdrs.setdefault("Content-Type", "application/json")
    req = urllib.request.Request(url, data=data, method=method, headers=hdrs)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, r.read().decode()
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode()
    except Exception as e:
        return None, f"<{type(e).__name__}: {e}>"


def rest_get(path):
    code, body = http("GET", REST + path)
    return code, json.loads(body) if body and not body.startswith("<") else None


def rest_post(path, obj):
    return http("POST", REST + path, json.dumps(obj))


def http_hdr(method, url, body=None, timeout=8, headers=None):
    """Variant of http() that also returns the response headers dict."""
    data = None if body is None else body.encode()
    hdrs = dict(headers or {})
    if body is not None:
        hdrs.setdefault("Content-Type", "application/json")
    req = urllib.request.Request(url, data=data, method=method, headers=hdrs)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, r.read().decode(), dict(r.headers)
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode(), dict(e.headers)
    except Exception as e:
        return None, f"<{type(e).__name__}: {e}>", {}


def parse_snapshot(headers):
    """Parse the X-NRM-Snapshot response header into a dict.

    Fields: ra/dec/st/tr/g/t (int) and ras/decs (float).  Returns None when the
    header is absent (production firmware without NRM_TEST_MODE).
    """
    raw = headers.get("X-NRM-Snapshot")
    if not raw:
        return None
    snap = {}
    for part in raw.split(";"):
        key, _, value = part.partition("=")
        snap[key] = int(value) if key in ("ra", "dec", "st", "tr", "g", "t") else float(value)
    return snap


def rest_post_snapshot(path, obj):
    """POST and return (code, body, snapshot parsed from the response header)."""
    code, body, headers = http_hdr("POST", REST + path, json.dumps(obj))
    return code, body, parse_snapshot(headers)


def alpaca(path, method="GET", params=None, form=None):
    qs = urllib.parse.urlencode(params) if params else ""
    url = f"{ALPACA}{T}{path}"
    if qs:
        url += ("&" if "?" in url else "?") + qs
    if method == "PUT" and form is not None:
        enc = urllib.parse.urlencode(form)
        return http("PUT", url, body=enc, headers={"Content-Type": "application/x-www-form-urlencoded"})
    return http(method, url)


def alpaca_val(path, **params):
    code, body = alpaca(path, params=params)
    try:
        return json.loads(body)
    except Exception:
        return None


def status():
    code, s = rest_get("/api/status")
    if s is None:
        raise MountError(f"status read failed (HTTP {code})")
    return s


def observe():
    """Normalised observable state — the single source of truth for assertions."""
    s = status()
    return {
        "status": s["status"],                # ready | slewing | tracking | parked | error
        "tracking": s["tracking"],            # none | sidereal | lunar | solar
        "power": s["power"],
        "is_home": s["is_home"],
        "ra_axis": s["debug"]["ra_axis_deg"],
        "dec_axis": s["debug"]["dec_axis_deg"],
        "ra_steps": s["debug"]["ra_steps"],
        "dec_steps": s["debug"]["dec_steps"],
        "guiding": s["debug"]["guiding"],
    }


def alpaca_state():
    """Alpaca-visible state properties."""
    def v(path, **p):
        r = alpaca_val(path, ClientID=1, ClientTransactionID=1, **p)
        return r.get("Value") if r else None
    return {
        "slewing": v("/slewing"),
        "atpark": v("/atpark"),
        "athome": v("/athome"),
        "tracking": v("/tracking"),
        "sideofpier": v("/sideofpier"),
        "ra": v("/rightascension"),
        "dec": v("/declination"),
    }


def wait_until(pred, timeout=30, interval=0.25, desc=""):
    """Poll until pred(observe()) is truthy; raise on timeout."""
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        last = observe()
        if pred(last):
            return last
        time.sleep(interval)
    raise MountError(f"timeout waiting for {desc or pred} (last={last})")


def settle(expected_status="ready", timeout=30):
    """Wait until the mount is not slewing and, if given, in expected_status."""
    return wait_until(lambda o: o["status"] != "slewing" and (expected_status is None or o["status"] == expected_status),
                      timeout=timeout, desc=f"status={expected_status}")


def set_time():
    """Make GOTO possible: mount_time_valid must be true."""
    rest_post("/api/settings", {"time": "2026-09-28T02:00:00Z",
                                "lat": -32.89, "lon": -68.83, "elevation": 750})


def home():
    """Return to mechanical home (0,0) and settle.  Idempotent."""
    rest_post("/api/unpark", {})  # leave PARKED before homing (home is rejected while parked)
    rest_post("/api/stop", {})
    rest_post("/api/home", {})
    settle("ready", timeout=40)


def assert_envelope(o):
    assert abs(o["ra_axis"]) <= BENCH_ENVELOPE_DEG + 1.0, \
        f"RA axis {o['ra_axis']}° outside bench envelope {BENCH_ENVELOPE_DEG}°"
    assert abs(o["dec_axis"]) <= BENCH_ENVELOPE_DEG + 1.0, \
        f"DEC axis {o['dec_axis']}° outside bench envelope {BENCH_ENVELOPE_DEG}°"


@pytest.fixture(scope="session", autouse=True)
def mount_session():
    """One bench session: ensure time + home, then home at teardown."""
    set_time()
    home()
    yield
    # Teardown: stop and home — never leave the bench mid-slew.
    try:
        rest_post("/api/unpark", {})
        rest_post("/api/stop", {})
        rest_post("/api/home", {})
        settle("ready", timeout=40)
    except Exception:
        pass
