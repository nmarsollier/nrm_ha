"""ctypes bindings for the host harness (libnrm_host.so).

The library compiles the production mount/motors C (scheduler, queue, motion
loop, RMT encoding) with RTOS/RMT/time mocked, so the tests below exercise the
real firmware code rather than a stub that fakes movement.
"""
import ctypes as C
import os
from pathlib import Path
import time

PROFILE = os.environ.get("NRM_TEST_PROFILE", "nrm_ha")
LIB = Path(__file__).resolve().parent / f"libhost_{PROFILE}.so"

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

# Guide rates are mount-agnostic.  The step scale is an INDEPENDENT expected
# value per profile (not read from the firmware): a wrong reduction must not
# shift the test expectations.  test_profile_contract.py separately checks the
# firmware's own conversion against this declared reduction.
SIDEREAL_DPS = 0.004178074
GUIDE_DPS = SIDEREAL_DPS * 0.5            # 0.002089037 — NOT 0.5 deg/s

_GEAR_RATIO = {"nrm_ha": 300, "bft": 500}[PROFILE]
DEG_PER_STEP = 1.8 / (64 * _GEAR_RATIO)


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
        self._bind("host_mount_park", C.c_int, [])
        self._bind("host_mount_unpark", C.c_int, [])
        self._bind("host_set_guide_rate_ra", C.c_int, [C.c_float])
        self._bind("host_set_guide_rate_dec", C.c_int, [C.c_float])
        self._bind("host_get_guide_rate_ra", C.c_float, [])
        self._bind("host_get_guide_rate_dec", C.c_float, [])
        self._bind("host_slew_duration", C.c_float, [C.c_float, C.c_float])
        self._bind("host_status", C.c_int, [])
        self._bind("host_tracking", C.c_int, [])
        self._bind("host_guiding", C.c_int, [])
        self._bind("host_guide_truncated", C.c_int, [])
        self._bind("host_position_uncertain", C.c_int, [])
        self._bind("host_has_hardware_fault", C.c_int, [])
        self._bind("host_manual_move", C.c_int, [])
        self._bind("host_saved_tracking", C.c_int, [])
        self._bind("host_move_axis_ra_dps", C.c_float, [])
        self._bind("host_move_axis_dec_dps", C.c_float, [])
        self._bind("host_move_axis_version", C.c_uint32, [])
        self._bind("host_manual_session", C.c_uint32, [])
        self._bind("host_finished_manual_session", C.c_uint32, [])
        self._bind("host_finished_manual_aborted", C.c_int, [])
        self._bind("host_stop_generation", C.c_uint32, [])
        self._bind("host_admission_closed", C.c_int, [])
        self._bind("host_idle_generation", C.c_uint32, [])
        self._bind("host_idle_aborted", C.c_int, [])
        self._bind("host_idle_aborted_generation", C.c_uint32, [])
        self._bind("host_fill_queue_with_slew", C.c_int, [C.c_int])
        self._bind("host_enqueue_slew", C.c_int, [C.c_float, C.c_float, C.c_float])
        self._bind("host_microsteps_active", C.c_int, [])
        self._bind("host_queue_front_type", C.c_int, [])
        self._bind("host_update_power", None, [C.c_int])
        self._bind("host_enter_hardware_fault", C.c_int, [])
        self._bind("host_tmc_set_reinit_fail", None, [C.c_int])
        self._bind("host_tmc_set_microsteps", None, [C.c_uint16])
        self._bind("host_tmc_set_microsteps_fail", None, [C.c_int])
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
        self._bind("host_dir_positive_level", C.c_int, [C.c_int])
        self._bind("host_dir_level_read", C.c_int, [C.c_int])
        self._bind("host_dir_level_set", None, [C.c_int, C.c_int])
        self._bind("host_rmt_set_controlled", C.c_int, [C.c_int, C.c_int])
        self._bind("host_rmt_emit", None, [C.c_int])
        self._bind("host_rmt_emit_one", None, [C.c_int])
        self._bind("host_rmt_emit_partial", None, [C.c_int, C.c_int])
        self._bind("host_rmt_complete", None, [C.c_int])
        self._bind("host_rmt_pending_pulses", C.c_int, [C.c_int])
        self._bind("host_rmt_pending_dir", C.c_int, [C.c_int])
        self._bind("host_rmt_transmit_raw", C.c_int, [C.c_int, C.c_int])
        self._bind("host_rmt_set_tx_error_after", None, [C.c_int, C.c_int])
        self._bind("host_rmt_arm_complete_barrier", None, [C.c_int])
        self._bind("host_rmt_reached_complete_barrier", C.c_int, [])
        self._bind("host_rmt_release_complete_barrier", None, [])
        self._bind("host_rmt_del_channel_entered", C.c_int, [])
        self._bind("host_rmt_freed_during_publication", C.c_int, [])
        self._bind("host_slew_accel_deg", C.c_float, [])
        self._bind("host_slew_decel_deg", C.c_float, [])
        self._bind("host_set_pause_after_enqueue", None, [C.c_int])
        self._bind("host_reached_enqueue_hook", C.c_int, [])
        self._bind("host_clear_enqueue_hook", None, [])
        self._bind("host_set_pause_before_enqueue", None, [C.c_int])
        self._bind("host_reached_before_enqueue_hook", C.c_int, [])
        self._bind("host_clear_before_enqueue_hook", None, [])
        self._bind("host_set_pause_after_process_command", None, [C.c_int])
        self._bind("host_reached_process_command_hook", C.c_int, [])
        self._bind("host_clear_process_command_hook", None, [])
        self._bind("host_set_pause_after_stop_begin", None, [C.c_int])
        self._bind("host_reached_stop_begin_hook", C.c_int, [])
        self._bind("host_clear_stop_begin_hook", None, [])
        self._bind("host_set_pause_after_move_axis_stop_open", None, [C.c_int])
        self._bind("host_reached_move_axis_stop_open_hook", C.c_int, [])
        self._bind("host_clear_move_axis_stop_open_hook", None, [])
        self._bind("host_set_pause_before_move_axis_tracking_transition", None, [C.c_int])
        self._bind("host_reached_before_move_axis_tracking_transition_hook", C.c_int, [])
        self._bind("host_clear_before_move_axis_tracking_transition_hook", None, [])
        self._bind("host_set_pause_before_move_axis_tracking_restore", None, [C.c_int])
        self._bind("host_reached_before_move_axis_tracking_restore_hook", C.c_int, [])
        self._bind("host_clear_before_move_axis_tracking_restore_hook", None, [])
        self._bind("host_set_pause_before_move_axis_saved_tracking_publish", None, [C.c_int])
        self._bind("host_reached_before_move_axis_saved_tracking_publish_hook", C.c_int, [])
        self._bind("host_clear_before_move_axis_saved_tracking_publish_hook", None, [])
        self._bind("host_set_pause_after_power_reinit", None, [C.c_int])
        self._bind("host_reached_after_power_reinit", C.c_int, [])
        self._bind("host_clear_after_power_reinit", None, [])
        self._bind("host_set_pause_before_idle_publish", None, [C.c_int])
        self._bind("host_reached_before_idle_publish", C.c_int, [])
        self._bind("host_clear_before_idle_publish", None, [])
        self._bind("host_set_pause_after_motion_loop", None, [C.c_int])
        self._bind("host_reached_after_motion_loop", C.c_int, [])
        self._bind("host_clear_after_motion_loop", None, [])
        self._bind("host_set_pause_drain_wait", None, [C.c_int])
        self._bind("host_reached_drain_wait", C.c_int, [])
        self._bind("host_clear_drain_wait", None, [])
        self._bind("host_set_pause_loop_top", None, [C.c_int])
        self._bind("host_reached_loop_top", C.c_int, [])
        self._bind("host_clear_loop_top", None, [])
        self._bind("host_arm_first_producer_gate", None, [C.c_int])
        self._bind("host_reached_first_producer_gate", C.c_int, [])
        self._bind("host_clear_first_producer_gate", None, [])
        self._bind("host_release_first_producer", None, [])
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
