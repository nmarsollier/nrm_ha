"""NRM-HA state machine — exhaustive transitions (black box).

For each state (ready/slewing/tracking/parked/error) every operation is applied
and both (1) the call result and (2) the resulting state observed via
/api/status are verified. The ERROR state is reached via the debug seam
(/api/debug/power), compiled only in TEST MODE.

Moves are bounded (±3°) and teardown returns to HOME.
"""
import json
import time

import pytest

from conftest import (observe, status, rest_post, rest_post_snapshot, rest_get,
                      alpaca, alpaca_val, settle, wait_until, home, set_time,
                      REST, ALPACA, T)

# ── Debug seam (TEST MODE) ───────────────────────────────────────

def debug_power(force, present=None):
    body = {"force": 1 if force else 0}
    if force and present is not None:
        body["present"] = 1 if present else 0
    rest_post("/api/debug/power", body)


def debug_fault():
    rest_post("/api/debug/fault", {"type": "hardware"})


def _debug_available():
    code, body = rest_post("/api/debug/power", {"force": 0})
    try:
        return code == 200 and json.loads(body).get("ok") is True
    except Exception:
        return False


DEBUG = _debug_available()

# ── bring-to-state ───────────────────────────────────────────────

def bring_to(state):
    if state == "ready":
        o = observe()
        if o["status"] == "error":
            recover()
        if o["status"] == "parked":
            rest_post("/api/unpark", {})
        if o["status"] in ("slewing", "tracking"):
            rest_post("/api/stop", {})
        settle("ready")
    elif state == "parked":
        rest_post("/api/stop", {})
        rest_post("/api/park", {})
        settle("parked")
    elif state == "tracking":
        bring_to("ready")
        rest_post("/api/tracking", {"tracking": "sidereal"})
        wait_until(lambda o: o["status"] == "tracking", timeout=5, desc="tracking")
        time.sleep(0.5)  # determinism: let the tracking loop settle
    elif state == "slewing":
        bring_to("ready")
        rest_post("/api/move-axis-speed", {"ra_rate": 1.0, "dec_rate": 0.0})
        wait_until(lambda o: o["status"] == "slewing", timeout=5, desc="slewing")
    elif state == "error":
        bring_to("ready")
        debug_power(True, False)
        wait_until(lambda o: o["status"] == "error", timeout=5, desc="error")
    else:
        raise ValueError(state)


def recover():
    """Clear a power-fake ERROR (recoverable) back to READY."""
    debug_power(False, False)
    wait_until(lambda o: o["status"] != "error", timeout=5, desc="recover")
    settle("ready")


# ── Operations (each returns True == accepted by the API) ────────

LAST_BODY = None


def _ok(code, body):
    global LAST_BODY
    LAST_BODY = body
    try:
        return code == 200 and json.loads(body).get("ok") is True
    except Exception:
        return False


def op_stop():
    code, body = rest_post("/api/stop", {})
    return _ok(code, body)


def op_park():
    code, body = rest_post("/api/park", {})
    return _ok(code, body)


def op_unpark():
    code, body = rest_post("/api/unpark", {})
    return _ok(code, body)


def op_home():
    code, body = rest_post("/api/home", {})
    return _ok(code, body)


def op_tracking_sidereal():
    code, body = rest_post("/api/tracking", {"tracking": "sidereal"})
    return _ok(code, body)


def op_tracking_off():
    code, body = rest_post("/api/tracking", {"tracking": "none"})
    return _ok(code, body)


def op_move_axis_ra():
    code, body = rest_post("/api/move-axis", {"axis": "ra", "degrees": 3.0, "speed": 1})
    return _ok(code, body)


def op_move_axis_speed():
    code, body = rest_post("/api/move-axis-speed", {"ra_rate": 1.0, "dec_rate": 0.0})
    return _ok(code, body)


def op_goto():
    # Target ~5° from the pole → small DEC move, bounded RA (near meridian).
    code, body = rest_post("/api/slew-to-coordinates", {"ra": 2.0, "dec": -85.0, "speed": 4})
    return _ok(code, body)


OPS = {
    "stop": op_stop,
    "park": op_park,
    "unpark": op_unpark,
    "home": op_home,
    "tracking_sidereal": op_tracking_sidereal,
    "tracking_off": op_tracking_off,
    "move_axis_ra": op_move_axis_ra,
    "move_axis_speed": op_move_axis_speed,
    "goto": op_goto,
}


# Expected duration (s) of each operation to reach its final state.  The
# timeout is duration × MARGIN + 1s — tight on purpose: a move that takes
# noticeably longer than expected is itself a failure signal.
OP_DURATION = {
    "stop": 1.0,
    "park": 1.0,
    "unpark": 1.0,
    "home": 15.0,            # worst case ~28° a 6°/s + rampa
    "tracking_sidereal": 1.5,
    "tracking_off": 1.0,
    "move_axis_ra": 8.0,     # 3° a 1°/s + rampa
    "move_axis_speed": 1.5,
    "goto": 15.0,            # ~28° a 6°/s + rampa
}
DURATION_MARGIN = 1.5


def settle_after(op, final):
    """Wait until the mount reaches the expected final status, within a tight
    bound derived from the operation's known duration."""
    timeout = OP_DURATION[op] * DURATION_MARGIN + 1.0
    return wait_until(lambda o: o["status"] == final, timeout=timeout,
                      desc=f"{op}->{final} ({timeout:.0f}s)")


# ── Transition matrix: (from, op, accept, final_status) ──────────
# 5 states × 9 operations = 45 exhaustive transitions.
# "queued" marks operations admitted but deferred behind a move-axis
# continuous (queue admission policy).
TRANSITIONS = [
    # READY — all operations accepted
    ("ready", "stop", True, "ready"),
    ("ready", "park", True, "parked"),
    ("ready", "unpark", True, "ready"),
    ("ready", "home", True, "ready"),
    ("ready", "tracking_sidereal", True, "tracking"),
    ("ready", "tracking_off", True, "ready"),
    ("ready", "move_axis_ra", True, "ready"),
    ("ready", "move_axis_speed", True, "slewing"),
    ("ready", "goto", True, "ready"),
    # PARKED — only unpark/park/stop exit or stay; the rest rejected
    ("parked", "stop", True, "parked"),
    ("parked", "park", True, "parked"),
    ("parked", "unpark", True, "ready"),
    ("parked", "home", False, "parked"),
    ("parked", "tracking_sidereal", False, "parked"),
    ("parked", "tracking_off", True, "parked"),   # stop() from parked does not unpark
    ("parked", "move_axis_ra", False, "parked"),
    ("parked", "move_axis_speed", False, "parked"),
    ("parked", "goto", False, "parked"),
    # TRACKING — stop/off/park end; goto/move pause and resume
    ("tracking", "stop", True, "ready"),
    ("tracking", "park", True, "parked"),
    ("tracking", "unpark", True, "tracking"),     # unpark outside parked = no-op
    ("tracking", "home", True, "ready"),          # home does NOT resume tracking
    ("tracking", "tracking_sidereal", True, "tracking"),
    ("tracking", "tracking_off", True, "ready"),
    ("tracking", "move_axis_ra", True, "tracking"),
    ("tracking", "move_axis_speed", True, "slewing"),
    ("tracking", "goto", True, "tracking"),
    # SLEWING (continuous move-axis) — stop/park/home/tracking_off interrupt
    ("slewing", "stop", True, "ready"),
    ("slewing", "park", True, "parked"),
    ("slewing", "unpark", True, "slewing"),            # no-op
    ("slewing", "home", True, "ready"),
    ("slewing", "tracking_sidereal", True, "slewing"),  # queued
    ("slewing", "tracking_off", True, "ready"),
    ("slewing", "move_axis_ra", True, "slewing"),       # queued
    ("slewing", "move_axis_speed", True, "slewing"),    # preempta
    ("slewing", "goto", True, "slewing"),               # queued
    # ERROR — nothing is accepted except teardown (reboot)
    ("error", "stop", False, "error"),
    ("error", "park", False, "error"),
    ("error", "unpark", False, "error"),
    ("error", "home", False, "error"),
    ("error", "tracking_sidereal", False, "error"),
    ("error", "tracking_off", False, "error"),
    ("error", "move_axis_ra", False, "error"),
    ("error", "move_axis_speed", False, "error"),
    ("error", "goto", False, "error"),
]


@pytest.fixture(autouse=True)
def _home_after_each():
    """Home the mount and clear any power override after every test."""
    yield
    try:
        debug_power(False, False)
        wait_until(lambda o: o["status"] != "error", timeout=6, desc="recover")
        rest_post("/api/unpark", {})
        rest_post("/api/stop", {})
        rest_post("/api/home", {})
        settle("ready", timeout=40)
    except Exception:
        pass


@pytest.mark.parametrize("from_state,op,accept,final", TRANSITIONS,
                         ids=[f"{a}--{b}->{d}" for a, b, c, d in TRANSITIONS])
def test_transition(from_state, op, accept, final):
    if from_state == "error" and not DEBUG:
        pytest.skip("board not in TEST MODE (no /api/debug/power)")
    bring_to(from_state)

    before = observe()
    assert before["status"] == from_state, f"no se pudo llevar a {from_state}: {before['status']}"

    got = OPS[op]()

    assert got == accept, \
        f"{op} from {from_state}: expected accept={accept}, got={got}, body={LAST_BODY}"

    after = settle_after(op, final)
    assert after["status"] == final, \
        f"{op} from {from_state}: final state {after['status']}, expected {final}"


# ── GOTO: final position verified via /status ─────────────────

def test_goto_reaches_target_and_reports_it():
    bring_to("ready")
    set_time()

    target_dec = -85.0
    code, body = rest_post("/api/slew-to-coordinates", {"ra": 2.0, "dec": target_dec, "speed": 4})
    assert _ok(code, body), f"GOTO rejected: {body}"

    settle("ready", timeout=40)

    o = observe()
    assert o["status"] == "ready", f"state after GOTO: {o['status']}"
    assert abs(o["ra_steps"]) > 0 or abs(o["dec_steps"]) > 0, "GOTO did not move the axes"

    # The celestial position reported by Alpaca must reach the target.
    dec = alpaca_val("/declination", ClientID=1, ClientTransactionID=1)
    assert dec and abs(dec["Value"] - target_dec) < 1.0, \
        f"final declination {dec and dec['Value']} != target {target_dec}"

    home()


# ── Park / Unpark / Home with real effect ─────────────────────────

def test_park_clears_tracking_and_blocks_motion():
    bring_to("ready")
    rest_post("/api/tracking", {"tracking": "sidereal"})
    wait_until(lambda o: o["status"] == "tracking", timeout=5, desc="tracking")

    rest_post("/api/park", {})
    settle("parked")
    o = observe()
    assert o["status"] == "parked"
    assert o["tracking"] == "none", "park did not disable tracking"
    assert alpaca_val("/atpark", ClientID=1, ClientTransactionID=1)["Value"] is True

    # Movement rejected from PARKED
    code, body = rest_post("/api/move-axis", {"axis": "ra", "degrees": 1.0, "speed": 1})
    assert not _ok(code, body), "movement accepted while parked"

    bring_to("ready")


def test_home_returns_to_mechanical_zero():
    bring_to("ready")
    # Move away from home with a bounded move
    rest_post("/api/move-axis", {"axis": "dec", "degrees": -2.0, "speed": 2})
    settle("ready")

    code, body = rest_post("/api/home", {})
    assert _ok(code, body), f"home rejected: {body}"
    settle("ready", timeout=40)

    o = observe()
    assert o["ra_axis"] == 0.0 and o["dec_axis"] == 0.0, \
        f"home did not return to (0,0): ra={o['ra_axis']} dec={o['dec_axis']}"


# ── Stop cancels active motion (global STOP ≠ AbortSlew) ─────

def test_stop_halts_move_axis_speed():
    bring_to("ready")
    rest_post("/api/move-axis-speed", {"ra_rate": 2.0, "dec_rate": 0.0})
    wait_until(lambda o: o["status"] == "slewing", timeout=5, desc="slewing")
    time.sleep(0.5)

    # The response header carries the position at the instant STOP was received
    # (before the business call) — the deterministic reference, no HTTP latency.
    code, body, snap = rest_post_snapshot("/api/stop", {})
    assert _ok(code, body), f"stop rejected: {body}"
    settle("ready")
    time.sleep(0.5)
    ra_after = observe()["ra_steps"]

    # STOP clamps the mount.  The position counter advances in whole 100-step
    # RMT batches, so the only possible post-STOP delta is 0 or one batch of
    # counter catch-up — never a continuous drift.
    assert abs(ra_after - snap["ra"]) <= 100, \
        f"RA moved after STOP: {snap['ra']} -> {ra_after}"


def test_stop_ra_does_not_stop_dec():
    """Per-axis independence: stopping RA must not stop DEC."""
    bring_to("ready")
    rest_post("/api/move-axis-speed", {"ra_rate": 1.0, "dec_rate": 1.0})
    wait_until(lambda o: o["status"] == "slewing", timeout=5, desc="slewing")
    time.sleep(0.5)

    # Stop only RA (rate=0) keeping DEC.  The header carries the position at
    # the instant the command was received.
    code, body, snap = rest_post_snapshot("/api/move-axis-speed",
                                          {"ra_rate": 0.0, "dec_rate": 1.0})
    assert _ok(code, body), f"move-axis-speed rejected: {body}"
    wait_until(lambda o: o["status"] == "slewing", timeout=5, desc="slewing")
    time.sleep(0.5)
    dec_after = observe()["dec_steps"]
    ra_after = observe()["ra_steps"]

    assert abs(ra_after - snap["ra"]) <= 100, \
        f"RA moved after stopping RA: {snap['ra']} -> {ra_after}"
    assert dec_after > snap["dec"], \
        f"DEC stopped when RA was stopped: {snap['dec']} -> {dec_after}"


# ── Latched fault (debug seam) ─────────────────────────────────

def test_hardware_fault_persists_until_reboot():
    assert DEBUG, "board not in TEST MODE (/api/debug/fault absent)"
    bring_to("ready")
    debug_fault()
    wait_until(lambda o: o["status"] == "error", timeout=5, desc="fault")

    # power-cycle does NOT clear a hardware fault
    debug_power(True, False)
    time.sleep(0.5)
    debug_power(True, True)
    time.sleep(0.5)
    assert observe()["status"] == "error", "fault de hardware fue limpiado por power-cycle"

    # Cleanup: the latched fault is only cleared by a reboot, but /api/reset
    # drops the USB NCM link and the board stops answering over HTTP.
    # "ready" cannot be observed over the API after the reboot; recovery
    # recovery is re-flash or re-plug. This test must be last.
    debug_power(False, False)
    rest_post("/api/reset", {})
