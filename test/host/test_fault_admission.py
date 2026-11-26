"""Host tests — PR5/E06: safe admission after hardware fault / failed drain.

Admission of new motion must stay CLOSED whenever a hardware fault is latched, a
drain fails, or a stop times out — even if STOP/PARK/UNPARK/power-return are then
attempted.  Only a successful confirmation of rest with no latched fault may
reopen admission.

``host_admission_closed()`` reads the admission gate ``s_stopping`` directly under
``motors_admission_lock`` (via ``motors_admission_is_closed``) — the unambiguous
observable for these tests, unlike a queue-full rejection.
"""
import threading
import time
import unittest

from harness import (Harness, STATUS_READY, STATUS_SLEWING, STATUS_TRACKING,
                     STATUS_PARKED, STATUS_ERROR, TRACKING_SIDEREAL, TRACKING_NONE)


class FaultAdmissionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.h = Harness()

    def setUp(self):
        self.h.reset()

    def assert_admission_closed(self):
        # Direct admission-gate observability (s_stopping under the admission
        # lock) — a full queue must not be mistaken for a closed gate.
        self.assertEqual(self.h.lib.host_admission_closed(), 1,
                         "admission must be closed")

    def assert_admission_open(self):
        self.assertEqual(self.h.lib.host_admission_closed(), 0,
                         "admission must be open")

    # ── Test 1: a latched hardware fault keeps admission closed ─────────────

    def test_stop_hardware_fault_keeps_admission_closed(self):
        h = self.h
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.5, 0.0), 1)
        h.wait_status(STATUS_SLEWING)

        # A real RMT transmit failure latches a hardware fault and enters ERROR.
        h.lib.host_rmt_set_tx_error(0)
        h.wait_status(STATUS_ERROR)

        self.assertEqual(h.lib.host_has_hardware_fault(), 1,
                         "the hardware fault must be latched")
        self.assert_admission_closed()

        # STOP must report the fault and must not reopen admission.
        self.assertEqual(h.lib.host_mount_stop(), 0, "STOP must report the fault")
        self.assertEqual(h.lib.host_status(), STATUS_ERROR)
        self.assert_admission_closed()

        # New motion is rejected (both at the mount layer and at the gate).
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.3, 0.0), 0)
        self.assert_admission_closed()

    # ── Test 2: a failed drain keeps admission closed ───────────────────────

    def test_failed_stop_drain_keeps_admission_closed(self):
        h = self.h
        # Absent RA completion makes the stop's graceful drain time out → abort.
        h.lib.host_rmt_set_absent(0)
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.5, 0.0), 1)
        h.wait_status(STATUS_SLEWING)

        self.assertEqual(h.lib.host_mount_stop(), 0,
                         "a failed drain must not report a clean stop")
        self.assertEqual(h.lib.host_position_uncertain(), 1,
                         "a failed drain must flag position uncertainty")
        self.assert_admission_closed()

        self.assertEqual(h.lib.host_mount_move_axis_speed(0.3, 0.0), 0,
                         "no new motion may be admitted after a failed drain")

    # ── Test 4: PARK does not publish PARKED or reopen admission on fault ────

    def test_park_fault_cannot_publish_parked_or_reopen_admission(self):
        h = self.h
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.5, 0.0), 1)
        h.wait_status(STATUS_SLEWING)

        h.lib.host_rmt_set_tx_error(0)
        h.wait_status(STATUS_ERROR)

        self.assertEqual(h.lib.host_mount_park(), 0, "PARK must report the fault")
        self.assertEqual(h.lib.host_status(), STATUS_ERROR,
                         "PARK must not overwrite ERROR with PARKED")
        self.assert_admission_closed()

    # ── Test 7: latched ERROR survives STOP, PARK, UNPARK and power return ───

    def test_latched_error_survives_stop_unpark_and_power_return(self):
        h = self.h
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.5, 0.0), 1)
        h.wait_status(STATUS_SLEWING)

        h.lib.host_rmt_set_tx_error(0)
        h.wait_status(STATUS_ERROR)
        steps_before_fault = h.lib.host_step_count()

        self.assertEqual(h.lib.host_mount_stop(), 0)
        self.assertEqual(h.lib.host_mount_park(), 0)
        self.assertEqual(h.lib.host_status(), STATUS_ERROR)

        # UNPARK must not clear a latched hardware fault either.
        self.assertEqual(h.lib.host_mount_unpark(), 0)
        self.assertEqual(h.lib.host_status(), STATUS_ERROR)

        # Power return must not clear a latched hardware fault.
        h.lib.host_update_power(0)
        h.lib.host_update_power(1)
        self.assertEqual(h.lib.host_has_hardware_fault(), 1,
                         "a latched hardware fault must survive power return")
        self.assertEqual(h.lib.host_status(), STATUS_ERROR)
        self.assert_admission_closed()

        self.assertEqual(h.lib.host_mount_move_axis_speed(0.3, 0.0), 0)
        self.assertEqual(h.lib.host_step_count(), steps_before_fault,
                         "no new pulses may be emitted while latched in ERROR")

    # ── Test 8: a failed manual finalization closes admission ───────────────

    def test_failed_manual_finalization_blocks_unsafe_admission(self):
        h = self.h
        self.assertEqual(h.lib.host_mount_tracking(TRACKING_SIDEREAL), 1)
        h.wait_status(STATUS_TRACKING)

        # Hold the executor just before the MoveAxis motion loop, so the RA
        # completion can be made absent deterministically before any batch runs.
        h.lib.host_set_pause_after_process_command(1)
        h.lib.host_clear_process_command_hook()

        self.assertEqual(h.lib.host_mount_move_axis_speed(0.5, 0.0), 1)
        h.wait_until(lambda: h.lib.host_reached_process_command_hook() == 1,
                     2.0, "executor processed the MoveAxis command")

        h.lib.host_rmt_set_absent(0)
        h.lib.host_set_pause_after_process_command(0)

        self.assertEqual(h.lib.host_saved_tracking(), TRACKING_SIDEREAL,
                         "the manual session must have saved tracking")

        # Single-axis zero → finalize (DEC already zero); the drain fails.
        self.assertEqual(h.lib.host_mount_move_axis_rate(0, 0.0), 0,
                         "the failed finalization must not report success")
        self.assertEqual(h.lib.host_tracking(), TRACKING_NONE,
                         "tracking must not be restored after a failed drain")
        self.assertEqual(h.lib.host_saved_tracking(), TRACKING_NONE)
        self.assertEqual(h.lib.host_position_uncertain(), 1)
        self.assert_admission_closed()

    # ── Test 10: healthy control operations reopen admission ────────────────

    def test_successful_control_operations_preserve_normal_admission(self):
        h = self.h
        self.assert_admission_open()

        self.assertEqual(h.lib.host_mount_stop(), 1)
        h.wait_status(STATUS_READY)
        self.assert_admission_open()

        # A normal manual finalization keeps admission open (E04).
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.5, 0.0), 1)
        h.wait_status(STATUS_SLEWING)
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.0, 0.0), 1)
        h.wait_status(STATUS_READY)
        self.assert_admission_open()

        # A normal move-axis order still works afterwards.
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.3, 0.0), 1)
        h.wait_status(STATUS_SLEWING)
        self.assertEqual(h.lib.host_mount_stop(), 1)
        h.wait_status(STATUS_READY)
        self.assert_admission_open()

    # ── E06-F: recoverable power loss restores safe admission ────────────────

    def test_recoverable_power_loss_restores_safe_motion_admission(self):
        h = self.h
        self.assert_admission_open()

        # A power loss is recoverable (not a hardware fault).
        h.lib.host_update_power(0)
        h.wait_status(STATUS_ERROR)
        self.assertEqual(h.lib.host_has_hardware_fault(), 0,
                         "a power loss is not a latched hardware fault")
        self.assert_admission_closed()

        # Power return re-initialises the drivers and restores READY + admission.
        h.lib.host_update_power(1)
        h.wait_status(STATUS_READY)
        self.assertEqual(h.lib.host_has_hardware_fault(), 0)
        self.assert_admission_open()

        # A legitimate movement works again without a reboot.
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.3, 0.0), 1)
        h.wait_status(STATUS_SLEWING)
        self.assertEqual(h.lib.host_mount_stop(), 1)
        h.wait_status(STATUS_READY)

    # ── E06-G: a failed manual drain closes admission at detection ───────────

    def test_failed_manual_drain_blocks_motion_before_requester_consumes_result(self):
        h = self.h
        # Absent RA completion from the start: the drain deterministically fails.
        h.lib.host_rmt_set_absent(0)
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.5, 0.0), 1)
        h.wait_status(STATUS_SLEWING)

        results = []

        def finalize():
            results.append(h.lib.host_mount_move_axis_speed(0.0, 0.0))

        t = threading.Thread(target=finalize)
        t.start()

        # The executor publishes the failed completion (and, with E06-G, closes
        # admission) before the requester consumes it.
        h.wait_until(lambda: h.lib.host_finished_manual_aborted() == 1, 5.0,
                     "executor published the failed drain")

        # Admission is already closed — independent of the requester consuming.
        self.assert_admission_closed()
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.3, 0.0), 0,
                         "no new motion may be admitted after the failed drain")

        t.join(timeout=5.0)
        self.assertEqual(results, [0], "the finalization must report the failure")
        self.assertEqual(h.lib.host_position_uncertain(), 1)

    # ── E06-E: the full-zero path reports a failed manual drain identically ──

    def test_failed_manual_finalization_full_zero_blocks_unsafe_admission(self):
        h = self.h
        self.assertEqual(h.lib.host_mount_tracking(TRACKING_SIDEREAL), 1)
        h.wait_status(STATUS_TRACKING)

        h.lib.host_set_pause_after_process_command(1)
        h.lib.host_clear_process_command_hook()

        self.assertEqual(h.lib.host_mount_move_axis_speed(0.5, 0.0), 1)
        h.wait_until(lambda: h.lib.host_reached_process_command_hook() == 1,
                     2.0, "executor processed the MoveAxis command")

        h.lib.host_rmt_set_absent(0)
        h.lib.host_set_pause_after_process_command(0)

        # Full-zero finalization (both axes) → the shared finalization fails.
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.0, 0.0), 0,
                         "the full-zero finalization must report the failure")
        self.assertEqual(h.lib.host_tracking(), TRACKING_NONE,
                         "tracking must not be restored after a failed drain")
        self.assertEqual(h.lib.host_position_uncertain(), 1)
        self.assert_admission_closed()


    # ── E06-I (Test A): power return waits for rest before re-initialising ───

    def test_power_return_does_not_reopen_admission_before_motion_is_reconciled(self):
        h = self.h
        # Hold the executor just before the motion loop, so it cannot confirm rest
        # while the power is restored.
        h.lib.host_set_pause_after_process_command(1)
        h.lib.host_clear_process_command_hook()

        self.assertEqual(h.lib.host_mount_move_axis_speed(0.5, 0.0), 1)
        h.wait_until(lambda: h.lib.host_reached_process_command_hook() == 1,
                     2.0, "executor processed the MoveAxis command")

        # Power loss: recoverable ERROR, admission closed.
        h.lib.host_update_power(0)
        h.wait_status(STATUS_ERROR)
        self.assert_admission_closed()

        # Power return in a thread: it must block on the rest barrier.
        returned = []

        def restore():
            h.lib.host_update_power(1)
            returned.append(True)

        t = threading.Thread(target=restore)
        t.start()
        time.sleep(0.05)  # let the recovery thread reach the rest barrier

        # The executor is still held, so recovery must not have completed.
        self.assertEqual(returned, [], "recovery must wait for rest confirmation")
        self.assertEqual(h.lib.host_status(), STATUS_ERROR,
                         "READY must not be published before rest")
        self.assert_admission_closed()

        # Release the executor: it aborts the motion and confirms rest.
        h.lib.host_set_pause_after_process_command(0)
        t.join(timeout=5.0)
        self.assertEqual(returned, [True], "recovery must complete after rest")

        h.wait_status(STATUS_READY)
        self.assert_admission_open()

        # A legitimate movement works after the recovery completed.
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.3, 0.0), 1)
        h.wait_status(STATUS_SLEWING)
        self.assertEqual(h.lib.host_mount_stop(), 1)
        h.wait_status(STATUS_READY)

    # ── E06-I (Test C): a TMC re-init failure latches a hardware fault ───────

    def test_hardware_fault_during_power_reinitialization_remains_latched(self):
        h = self.h
        # Make the TMC re-initialisation fail on power return.
        h.lib.host_tmc_set_reinit_fail(1)

        h.lib.host_update_power(0)
        h.wait_status(STATUS_ERROR)
        self.assert_admission_closed()

        # Power return: re-init fails → hardware fault latched, admission closed.
        h.lib.host_update_power(1)
        self.assertEqual(h.lib.host_has_hardware_fault(), 1,
                         "a failed re-init must latch a hardware fault")
        self.assertEqual(h.lib.host_status(), STATUS_ERROR)
        self.assert_admission_closed()

        self.assertEqual(h.lib.host_mount_move_axis_speed(0.3, 0.0), 0,
                         "no motion may be admitted after a latched fault")

        h.lib.host_tmc_set_reinit_fail(0)


    # ── E06-I: a concurrent hardware fault wins over the recovery ───────────

    def test_concurrent_hardware_fault_cannot_be_overwritten_by_power_recovery(self):
        h = self.h
        # Recoverable power loss: ERROR + admission closed.
        h.lib.host_update_power(0)
        h.wait_status(STATUS_ERROR)
        self.assert_admission_closed()

        # Suspend the recovery after a successful TMC re-init, before READY.
        h.lib.host_set_pause_after_power_reinit(1)
        h.lib.host_clear_after_power_reinit()

        returned = []

        def restore():
            h.lib.host_update_power(1)
            returned.append(True)

        t = threading.Thread(target=restore)
        t.start()
        h.wait_until(lambda: h.lib.host_reached_after_power_reinit() == 1, 2.0,
                     "recovery reached the post-reinit hook")

        # A hardware fault is latched while the recovery is suspended.
        h.lib.host_enter_hardware_fault()
        self.assertEqual(h.lib.host_has_hardware_fault(), 1)
        self.assertEqual(h.lib.host_status(), STATUS_ERROR)
        self.assert_admission_closed()

        # Resume the recovery: it must not overwrite the latched fault.
        h.lib.host_set_pause_after_power_reinit(0)
        t.join(timeout=5.0)

        self.assertEqual(h.lib.host_has_hardware_fault(), 1,
                         "the hardware fault must remain latched")
        self.assertEqual(h.lib.host_status(), STATUS_ERROR,
                         "READY must not overwrite ERROR")
        self.assert_admission_closed()

        self.assertEqual(h.lib.host_mount_move_axis_speed(0.3, 0.0), 0,
                         "no motion may be admitted after a latched fault")


    # ── E06: the recovery's READY+reopen is serialized with STOP ────────────

    def test_power_recovery_cannot_reopen_admission_during_new_stop(self):
        h = self.h
        h.lib.host_update_power(0)
        h.wait_status(STATUS_ERROR)

        # Suspend the recovery after a successful re-init, before the final
        # READY+reopen transition (which is now one atomic step under the control
        # mutex).
        h.lib.host_set_pause_after_power_reinit(1)
        h.lib.host_clear_after_power_reinit()

        returned = []

        def restore():
            h.lib.host_update_power(1)
            returned.append(True)

        t_restore = threading.Thread(target=restore)
        t_restore.start()
        h.wait_until(lambda: h.lib.host_reached_after_power_reinit() == 1, 2.0,
                     "recovery reached the post-reinit hook")

        # While the recovery is suspended, ERROR still persists, so a STOP is
        # rejected rather than racing the recovery's READY+reopen.
        stop_results = []

        def stopper():
            stop_results.append(h.lib.host_mount_stop())

        t_stop = threading.Thread(target=stopper)
        t_stop.start()
        t_stop.join(timeout=5.0)
        self.assertEqual(stop_results, [0],
                         "STOP must be rejected while ERROR still persists")

        # Release the recovery: it completes READY+reopen atomically.
        h.lib.host_set_pause_after_power_reinit(0)
        t_restore.join(timeout=5.0)
        self.assertEqual(returned, [True], "recovery must complete")

        h.wait_status(STATUS_READY)
        self.assert_admission_open()

        # A subsequent STOP works normally — the recovery did not leave admission
        # inconsistent.
        self.assertEqual(h.lib.host_mount_stop(), 1)
        h.wait_status(STATUS_READY)
        self.assert_admission_open()


if __name__ == "__main__":
    unittest.main()
