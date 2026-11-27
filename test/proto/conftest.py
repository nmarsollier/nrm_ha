"""Fixtures and helpers for the unified NRM-HA / BFT serial protocol suite.

Test selection is driven by the capabilities the mount declares via CAPABILITIES:

  - ``caps``            what the firmware declares (one round-trip).
  - ``expected_profile`` the *expected* values, used ONLY by test_capabilities.py
                       and by the identity resolution below.
  - ``suite_config``    run configuration and tolerances (profile.json).

A test declares its requirements with ``@pytest.mark.requires`` (boolean
capabilities) and ``@pytest.mark.geometry`` (declared geometry); when the
connected mount does not satisfy them the test is skipped with a reason.  Every
value a non-CAPABILITIES test computes comes from ``caps`` (which now publishes
the geometry, mechanical scale, slew ramp and limits) or the effective mount
state — never from hardcoded per-mount values.
"""
import json
import os
import time

import pytest

from client import ProtoClient, detect_serial_port
from oracle import angular_separation_deg, goto_time
import profiles

PROFILE_PATH = os.path.join(os.path.dirname(__file__), "profile.json")


# ── suite config (profile.json: serial port, tolerances) ───────────

class SuiteConfig(dict):
    @classmethod
    def load(cls, path=PROFILE_PATH):
        with open(path) as f:
            return cls(json.load(f))


@pytest.fixture(scope="session")
def suite_config():
    cfg = SuiteConfig.load()
    port = os.environ.get("NRM_SERIAL") or detect_serial_port() or cfg.get("serial_port")
    if port:
        cfg["serial_port"] = port
    else:
        pytest.fail(
            "No CDC serial port found. Connect the board, flash the firmware "
            "(make flash), then re-run."
        )
    return cfg


@pytest.fixture(scope="session")
def proto(suite_config):
    return ProtoClient(suite_config["serial_port"])


@pytest.fixture(scope="session")
def caps(proto):
    """The mount's declared capabilities (single CAPABILITIES round-trip)."""
    return proto.capabilities()


@pytest.fixture(scope="session")
def expected_profile(caps):
    """Resolve and validate the connected mount's identity (profiles.py).

    Identity is validated at session setup, before any fixture may home, change
    configuration or start a move.  The build/CLI profile name (``nrm_ha``) is
    mapped to the protocol ``device_id`` (``nrm-ha``) in this single place.
    """
    device_id = caps["device_id"]
    requested = os.environ.get("NRM_TEST_PROFILE")
    if requested:
        by_build = {p["build_profile"]: p for p in profiles.PROFILES.values()}
        if requested not in by_build:
            pytest.exit(f"NRM_TEST_PROFILE must be 'nrm_ha' or 'bft' (got {requested!r})")
        if by_build[requested]["device_id"] != device_id:
            pytest.exit(f"requested profile {requested!r} but connected device is {device_id!r}")
    if device_id not in profiles.PROFILES:
        pytest.exit(f"unknown device_id {device_id!r} (expected nrm-ha or bft)")
    return profiles.PROFILES[device_id]


# ── capability-driven selection ─────────────────────────────────────

def pytest_configure(config):
    config.addinivalue_line(
        "markers", "requires(*caps): run only if the mount declares every listed capability")
    config.addinivalue_line(
        "markers", "geometry(*geoms): run only on the listed declared geometry")


def _incompatible_reason(node, caps):
    """Return a skip reason when the node's markers are incompatible with caps."""
    declared = caps.get("caps", {})
    marker = node.get_closest_marker("requires")
    if marker:
        missing = [c for c in marker.args if not declared.get(c)]
        if missing:
            return f"mount does not declare required capability: {missing}"
    marker = node.get_closest_marker("geometry")
    if marker:
        geometry = caps.get("geometry")
        if geometry not in marker.args:
            return f"geometry is {geometry!r}, this test requires {marker.args}"
    return None


@pytest.fixture(autouse=True)
def _enforce_capabilities(request, caps):
    reason = _incompatible_reason(request.node, caps)
    if reason:
        pytest.skip(reason)


# ── bounded polling ───────────────────────────────────────────────

def wait_until(getter, pred, timeout, desc, poll=0.25):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        last = getter()
        if pred(last):
            return last
        time.sleep(poll)
    raise AssertionError(f"timeout waiting for {desc} (last={last!r})")


def wait_state(proto, pred, timeout, desc):
    return wait_until(lambda: proto.state()["state"], pred, timeout, desc)


def wait_slewing(proto, timeout=10):
    """Wait until the mount reports SLEWING (a command was picked up)."""
    return wait_state(proto, lambda s: s == "slewing", timeout, "state=slewing")


def wait_not_moving(proto, timeout=90):
    """Wait until the mount is idle and stays idle across one poll."""
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        last = proto.state()
        if last["state"] in ("ready", "tracking", "parked"):
            time.sleep(0.25)
            last = proto.state()
            if last["state"] in ("ready", "tracking", "parked"):
                return
        else:
            time.sleep(0.25)
    raise AssertionError(
        "timeout waiting for mount to stop moving "
        f"(last state={last.get('state')!r}, tracking={last.get('tracking')!r}, "
        f"guiding={last.get('guiding')!r}, "
        f"ra_axis={last.get('ra_axis_deg')!r}, dec_axis={last.get('dec_axis_deg')!r}, "
        f"position_uncertain={last.get('position_uncertain')!r}, "
        f"pier_side={last.get('pier_side')!r})"
    )


# ── site/time setup (GOTO requirement) ────────────────────────────

def setup_site_time(proto):
    """Set a valid site and UTC via the protocol to enable GOTO."""
    proto.config_set(lat=-32.89, lon=-68.83, elevation=750,
                     utc="2026-09-28T02:00:00Z")


# ── celestial / axis helpers ──────────────────────────────────────

def assert_angular_close(ra1, dec1, ra2, dec2, tol_deg, ctx=""):
    """Spherical angular distance between two equatorial positions."""
    sep = angular_separation_deg(ra1, dec1, ra2, dec2)
    assert sep <= tol_deg, f"{ctx} separation {sep:.4f}° > tolerance {tol_deg}°"


def axis_position(proto):
    """Physical axis position from the protocol STATE snapshot."""
    st = proto.state()
    return {
        "ra_deg": st["ra_axis_deg"],
        "dec_deg": st["dec_axis_deg"],
        "ra_steps": st["ra_steps"],
        "dec_steps": st["dec_steps"],
    }


def deg_per_step(caps):
    """Angular step scale from the mechanical parameters declared in CAPABILITIES."""
    return caps["full_step_deg"] / (caps["canonical_microsteps"] * caps["gear_ratio"])


def celestial_dec_at_dec_axis(dec_axis_deg):
    """Celestial DEC whose positive-D axis position is ``dec_axis_deg``.

    Both geometries place dec_axis = celestial DEC + 90 on the positive-D side
    (German pierEast: dec_axis = DEC + 90; fork south: D = 90 − p·dec = 90 + dec).
    """
    return dec_axis_deg - 90.0


def move_dec_off_pole(proto, degrees=30.0):
    """Home, then move DEC away from the reference/pole position."""
    proto.action("home")
    wait_not_moving(proto)
    proto.action("move", axis="dec", degrees=degrees, speed=4)
    wait_not_moving(proto)


def goto_and_arrive(proto, ra, dec, speed=4):
    """Issue a goto and wait until the mount is idle again."""
    proto.action("goto", ra=ra, dec=dec, speed=speed)
    wait_not_moving(proto)


def wait_tracking(proto, mode, timeout=10):
    """Wait until the protocol reports the given tracking mode."""
    return wait_until(lambda: proto.state()["tracking"],
                      lambda t: t == mode, timeout, f"tracking={mode}")


def assert_goto_duration(proto, caps, ra, dec, speed_dps=None, speed_rate=4,
                         slack_s=3.0, factor=1.35):
    """Issue a goto and assert the wall-clock duration matches the slew model.

    ``speed_dps`` defaults to the declared speed for ``speed_rate``; the ramp
    window comes from the ``slew`` object CAPABILITIES must declare.
    """
    if speed_dps is None:
        speed_dps = caps["slew_speeds_dps"][speed_rate - 1]
    ramp = caps["slew"]

    start = axis_position(proto)
    t0 = time.monotonic()
    proto.action("goto", ra=ra, dec=dec, speed=speed_rate)
    wait_not_moving(proto)
    elapsed = time.monotonic() - t0
    end = axis_position(proto)

    ra_dist = abs(end["ra_deg"] - start["ra_deg"])
    dec_dist = abs(end["dec_deg"] - start["dec_deg"])
    expected = goto_time(ra_dist, dec_dist, speed_dps, ramp)

    st = proto.state()
    assert_angular_close(st["ra"], st["dec"], ra, dec, 0.1, "goto arrival")

    lower = expected * 0.5
    upper = expected * factor + slack_s
    assert elapsed >= lower, \
        f"goto finished in {elapsed:.2f}s, suspiciously < {lower:.2f}s"
    assert elapsed <= upper, \
        f"goto took {elapsed:.2f}s, more than expected {upper:.2f}s"
    return {"elapsed": elapsed, "expected": expected,
            "ra_dist": ra_dist, "dec_dist": dec_dist,
            "final_ra": st["ra"], "final_dec": st["dec"],
            "start_axis": start, "end_axis": end}


# ── connection / module fixtures ──────────────────────────────────

@pytest.fixture(scope="module", autouse=True)
def _home_each_module(request, proto, caps, expected_profile):
    """Home once per test module so motion tests start from a known position.

    The module's markers are evaluated first, and so are the individual tests'
    markers when every test in the module is incompatible — so no HOME or limit
    change runs for a module that will be entirely skipped.
    """
    module = request.node
    reason = _incompatible_reason(module, caps)
    if reason is None:
        items = [i for i in request.session.items
                 if i.nodeid.startswith(module.nodeid + "::")]
        if items and all(_incompatible_reason(i, caps) for i in items):
            reason = "all tests in this module are incompatible with the connected mount"
    if reason:
        pytest.skip(reason)
    try:
        proto.action("unpark")
        proto.action("stop")
        proto.action("home")
        wait_not_moving(proto, timeout=40)
        proto.action("limits", param="clear_limits")
    except Exception:
        pass


@pytest.fixture(autouse=True)
def _settle(proto, expected_profile):
    """Start every test from a known READY state (no motion, no manual rates)."""
    try:
        proto.action("stop")
        proto.control(manual_ra_dps=0.0, manual_dec_dps=0.0)
    except Exception:
        pass   # the link may be stale after a reconnect
    yield
    try:
        proto.action("stop")
        proto.control(manual_ra_dps=0.0, manual_dec_dps=0.0)
    except Exception:
        pass
