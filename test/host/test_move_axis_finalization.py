"""Host tests — PR5/E04: finalizing MoveAxis must not reuse global STOP.

``motors_move_axis_stop()`` currently calls ``motors_stop_and_wait()``, which
closes global admission, advances the STOP generation, clears the whole command
queue and cancels guiding.  These are global-stop concerns, not manual-session
finalization concerns.  A normal MoveAxis finalization must only invalidate its
own session and reconcile its own committed pulses, leaving unrelated accepted
work intact and the global STOP generation unchanged.
"""
import time
import threading
import unittest

from harness import (Harness, STATUS_READY, STATUS_SLEWING, STATUS_TRACKING,
                     TRACKING_SIDEREAL, TRACKING_NONE)


class MoveAxisFinalizationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.h = Harness()

    def setUp(self):
        self.h.reset()

    def test_manual_finalization_does_not_advance_global_stop_generation(self):
        h = self.h
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.5, 0.0), 1)
        h.wait_status(STATUS_SLEWING)
        gen_before = h.lib.host_stop_generation()

        self.assertEqual(h.lib.host_mount_move_axis_speed(0.0, 0.0), 1)
        h.wait_status(STATUS_READY)

        self.assertEqual(h.lib.host_stop_generation(), gen_before,
                         "manual finalization must not advance the global STOP generation")
        self.assertEqual(h.lib.host_manual_move(), 0,
                         "the manual session must be invalidated")
        self.assertEqual(h.lib.host_move_axis_ra_dps(), 0.0,
                         "the manual RA setpoint must be zeroed")

    def test_manual_finalization_preserves_unrelated_accepted_commands(self):
        h = self.h
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.5, 0.0), 1)
        h.wait_status(STATUS_SLEWING)

        # An independent TRACK command is legitimately admitted while the manual
        # session is active (it stays queued behind the MoveAxis).
        self.assertEqual(h.lib.host_mount_tracking(TRACKING_SIDEREAL), 1)

        # Finalize the manual session.
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.0, 0.0), 1)

        # The unrelated TRACK command must survive and execute.
        h.wait_status(STATUS_TRACKING)
        self.assertEqual(h.lib.host_tracking(), TRACKING_SIDEREAL,
                         "finalization must not clear unrelated accepted commands")

        self.assertEqual(h.lib.host_mount_stop(), 1)

    def test_pending_manual_session_is_cancelled_without_global_stop(self):
        h = self.h
        gen_before = h.lib.host_stop_generation()

        # Admit and immediately finalize a manual session.
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.5, 0.0), 1)
        self.assertEqual(h.lib.host_manual_move(), 1)
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.0, 0.0), 1)
        h.wait_idle_status()

        self.assertEqual(h.lib.host_stop_generation(), gen_before,
                         "cancelling a session must not advance the generation")
        self.assertEqual(h.lib.host_manual_move(), 0,
                         "the manual session must be invalidated")
        self.assertEqual(h.lib.host_move_axis_ra_dps(), 0.0,
                         "the cancelled setpoint must be zeroed")

    def test_manual_finalization_accounts_committed_rmt_batches(self):
        h = self.h
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.5, 0.0), 1)
        h.wait_status(STATUS_SLEWING)
        time.sleep(0.15)  # let batches be committed

        self.assertEqual(h.lib.host_mount_move_axis_speed(0.0, 0.0), 1)
        h.wait_status(STATUS_READY)

        # Fine resolution: one STEP pulse == one canonical microstep.
        self.assertEqual(h.lib.host_ra_steps(), len(h.ra_steps_recorded()),
                         "RA counter != emitted RA pulses")
        self.assertEqual(h.lib.host_position_uncertain(), 0,
                         "graceful finalization must not flag uncertainty")

    def test_manual_finalization_restores_tracking_without_global_stop(self):
        h = self.h
        self.assertEqual(h.lib.host_mount_tracking(TRACKING_SIDEREAL), 1)
        h.wait_status(STATUS_TRACKING)
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.5, 0.0), 1)
        h.wait_status(STATUS_SLEWING)
        self.assertEqual(h.lib.host_saved_tracking(), TRACKING_SIDEREAL)
        gen_before = h.lib.host_stop_generation()

        self.assertEqual(h.lib.host_mount_move_axis_speed(0.0, 0.0), 1)
        h.wait_status(STATUS_TRACKING)
        self.assertEqual(h.lib.host_tracking(), TRACKING_SIDEREAL,
                         "finalization must restore the saved tracking")
        self.assertEqual(h.lib.host_stop_generation(), gen_before,
                         "manual finalization must not advance the global STOP generation")

        self.assertEqual(h.lib.host_mount_stop(), 1)

    def test_global_stop_supersedes_manual_finalization(self):
        h = self.h
        self.assertEqual(h.lib.host_mount_tracking(TRACKING_SIDEREAL), 1)
        h.wait_status(STATUS_TRACKING)
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.5, 0.0), 1)
        h.wait_status(STATUS_SLEWING)
        self.assertEqual(h.lib.host_saved_tracking(), TRACKING_SIDEREAL)

        # A global STOP while the manual session is active invalidates it and
        # cancels any pending restore.
        self.assertEqual(h.lib.host_mount_stop(), 1)
        self.assertEqual(h.lib.host_status(), STATUS_READY)
        self.assertEqual(h.lib.host_tracking(), TRACKING_NONE)
        self.assertEqual(h.lib.host_saved_tracking(), TRACKING_NONE)

        # A subsequent redundant zero must not reactivate anything.
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.0, 0.0), 1)
        self.assertEqual(h.lib.host_tracking(), TRACKING_NONE,
                         "no restore may survive a later STOP")

    def test_redundant_manual_zero_does_not_affect_unrelated_motion(self):
        h = self.h
        # Tracking active, no manual session: a zero must be a no-op.
        self.assertEqual(h.lib.host_mount_tracking(TRACKING_SIDEREAL), 1)
        h.wait_status(STATUS_TRACKING)
        gen_before = h.lib.host_stop_generation()

        self.assertEqual(h.lib.host_mount_move_axis_speed(0.0, 0.0), 1)
        self.assertEqual(h.lib.host_stop_generation(), gen_before,
                         "a redundant zero must not advance the STOP generation")
        self.assertEqual(h.lib.host_tracking(), TRACKING_SIDEREAL,
                         "a redundant zero must not stop tracking")
        self.assertEqual(h.lib.host_manual_move(), 0)

        self.assertEqual(h.lib.host_mount_stop(), 1)


if __name__ == "__main__":
    unittest.main()
