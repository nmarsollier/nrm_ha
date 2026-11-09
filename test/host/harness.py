"""ctypes bindings for the host harness (libnrm_host.so).

The library compiles the production mount/motors C (scheduler, queue, motion
loop, RMT encoding) with RTOS/RMT/time mocked, so the tests below exercise the
real firmware code rather than a stub that fakes movement.
"""
import ctypes as C
from pathlib import Path
import time

LIB = Path(__file__).resolve().parent / "libnrm_host.so"

# motors.h enums
STATUS_READY = 0
STATUS_SLEWING = 1
STATUS_TRACKING = 2
STATUS_PARKED = 3
STATUS_ERROR = 4

TRACKING_NONE = 0
TRACKING_SIDEREAL = 1
TRACKING_LUNAR = 2
TRACKING_SOLAR = 3

# mount.h GuideDirection
NORTH = 0
SOUTH = 1
EAST = 2
WEST = 3

# Geometry (NRM-HA: 64 microsteps, 300:1 reduction)
SIDEREAL_DPS = 0.004178074
GUIDE_DPS = SIDEREAL_DPS * 0.5            # 0.002089037 — NOT 0.5 deg/s
DEG_PER_STEP = 1.8 / (64 * 300)           # 0.00009375 deg


class HostStep(C.Structure):
    _fields_ = [
        ("axis", C.c_int),
        ("t_us", C.c_int64),
        ("dir_gpio", C.c_int),
        ("num_symbols", C.c_uint32),
        ("dur0", C.c_uint16),
        ("dur1", C.c_uint16),
        ("lvl0", C.c_uint8),
        ("lvl1", C.c_uint8),
    ]


class Harness:
    def __init__(self):
        self.lib = C.CDLL(str(LIB))
        self._bind("host_init", None, [])
        self._bind("host_reset", None, [])
        self._bind("host_wait_idle", None, [])
        self._bind("host_mount_guide", C.c_int, [C.c_int, C.c_uint32])
        self._bind("host_mount_tracking", C.c_int, [C.c_int])
        self._bind("host_mount_move_axis_speed", C.c_int, [C.c_float, C.c_float])
        self._bind("host_mount_move_axis_rate", C.c_int, [C.c_int, C.c_float])
        self._bind("host_mount_stop", C.c_int, [])
        self._bind("host_set_guide_rate_ra", C.c_int, [C.c_float])
        self._bind("host_set_guide_rate_dec", C.c_int, [C.c_float])
        self._bind("host_get_guide_rate_ra", C.c_float, [])
        self._bind("host_get_guide_rate_dec", C.c_float, [])
        self._bind("host_status", C.c_int, [])
        self._bind("host_tracking", C.c_int, [])
        self._bind("host_guiding", C.c_int, [])
        self._bind("host_guide_truncated", C.c_int, [])
        self._bind("host_has_hardware_fault", C.c_int, [])
        self._bind("host_update_power", None, [C.c_int])
        self._bind("host_ra_steps", C.c_int64, [])
        self._bind("host_dec_steps", C.c_int64, [])
        self._bind("host_set_ra_deg", None, [C.c_float])
        self._bind("host_set_dec_deg", None, [C.c_float])
        self._bind("host_rmt_set_tx_error", None, [C.c_int])
        self._bind("host_rmt_set_absent", None, [C.c_int])
        self._bind("host_rmt_set_suppress", None, [C.c_int])
        self._bind("host_rmt_clear_faults", None, [])
        self._bind("host_step_count", C.c_int, [])
        self._bind("host_step_get", None, [C.c_int, C.POINTER(HostStep)])
        self.lib.host_init()

    def _bind(self, name, restype, argtypes):
        fn = getattr(self.lib, name)
        fn.restype = restype
        fn.argtypes = argtypes

    def reset(self):
        self.lib.host_reset()

    # ── RMT STEP observability ──
    def step_records(self):
        n = self.lib.host_step_count()
        out = []
        for i in range(n):
            s = HostStep()
            self.lib.host_step_get(i, C.byref(s))
            out.append(s)
        return out

    def dec_steps_recorded(self):
        return [s for s in self.step_records() if s.axis == 1]

    def ra_steps_recorded(self):
        return [s for s in self.step_records() if s.axis == 0]

    # ── polling ──
    def wait_until(self, pred, timeout, desc):
        deadline = time.monotonic() + timeout
        last = None
        while time.monotonic() < deadline:
            last = pred()
            if last:
                return last
            time.sleep(0.01)
        raise AssertionError(f"timeout waiting for {desc} (last={last!r})")

    def wait_status(self, status, timeout=5.0):
        return self.wait_until(lambda: self.lib.host_status() == status,
                               timeout, f"status={status}")

    def wait_idle_status(self, timeout=5.0):
        """Wait until status is READY or TRACKING (motion loop quiescent)."""
        return self.wait_until(
            lambda: self.lib.host_status() in (STATUS_READY, STATUS_TRACKING),
            timeout, "status=ready|tracking")

    def wait_not_guiding(self, timeout=5.0):
        return self.wait_until(lambda: self.lib.host_guiding() == 0,
                               timeout, "guiding=false")
