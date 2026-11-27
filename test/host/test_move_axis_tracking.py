"""Host tests — PR5/E02: a rejected MoveAxis must not disturb tracking or guiding.

The producer (`motors_set_move_axis_speed`) used to call `motors_stop()` to pause
tracking *before* attempting to admit the MoveAxis.  If the enqueue was then
rejected (queue full), the stop had already interrupted tracking, cancelled
guiding and advanced the STOP generation — and the rollback did not reverse all
of it.

These tests drive the real production MoveAxis/admission paths.  They use the
weak `motors_test_hook_before_move_axis_enqueue` hook to hold the producer
deterministically before the enqueue (where a rejected order must leave the
mount untouched), and fill the queue with real commands to force a genuine
queue-full rejection.
"""
import threading
import unittest

from harness import (Harness, STATUS_READY, STATUS_SLEWING, STATUS_TRACKING,
                     TRACKING_SIDEREAL, TRACKING_NONE, EAST)


class MoveAxisTrackingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.h = Harness()

    def setUp(self):
        self.h.reset()

    def test_rejected_move_axis_preserves_active_tracking(self):
        h = self.h
        # Pause the executor right after it processes the TRACK command (before
        # it enters the tracking loop), so the command queue can be filled
        # deterministically without the tracking loop draining it.
        h.lib.host_set_pause_after_process_command(1)
        h.lib.host_clear_process_command_hook()
        self.assertEqual(h.lib.host_mount_tracking(TRACKING_SIDEREAL), 1)
        h.wait_until(lambda: h.lib.host_reached_process_command_hook() == 1,
                     2.0, "executor reached the process-command hook")
        gen_before = h.lib.host_stop_generation()
        self.assertEqual(h.lib.host_tracking(), TRACKING_SIDEREAL)
        self.assertEqual(h.lib.host_manual_move(), 0)

        # Fill the queue (the executor is paused, so the orders stay queued).
        self.assertEqual(h.lib.host_fill_queue_with_slew(10), 10)

        # A MoveAxis request now hits a genuine queue-full rejection.
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.5, 0.0), 0,
                         "the MoveAxis must be rejected (BUSY)")
        self.assertEqual(h.lib.host_stop_generation(), gen_before,
                         "a rejected MoveAxis must not advance the STOP generation")
        self.assertEqual(h.lib.host_tracking(), TRACKING_SIDEREAL,
                         "a rejected MoveAxis must not interrupt tracking")
        self.assertEqual(h.lib.host_manual_move(), 0,
                         "a rejected MoveAxis must not open a manual session")

        h.lib.host_set_pause_after_process_command(0)

    def test_rejected_move_axis_preserves_pending_pulse_guide(self):
        h = self.h
        self.assertEqual(h.lib.host_mount_tracking(TRACKING_SIDEREAL), 1)
        h.wait_status(STATUS_TRACKING)

        # A long guide pulse, observable across the whole MoveAxis attempt.
        self.assertEqual(h.lib.host_mount_guide(EAST, 5000), 1)
        h.wait_until(lambda: h.lib.host_guiding() == 1, 2.0, "guide active")

        h.lib.host_set_pause_before_enqueue(1)
        h.lib.host_clear_before_enqueue_hook()

        results = []

        def producer():
            results.append(h.lib.host_mount_move_axis_speed(0.5, 0.0))

        t = threading.Thread(target=producer)
        t.start()
        try:
            h.wait_until(lambda: h.lib.host_reached_before_enqueue_hook() == 1,
                         2.0, "producer reached the before-enqueue hook")

            # The guide must not be cancelled before the MoveAxis is admitted.
            self.assertEqual(h.lib.host_guiding(), 1,
                             "the MoveAxis must not cancel the guide before admission")

            self.assertEqual(h.lib.host_fill_queue_with_slew(10), 10)
            h.lib.host_set_pause_before_enqueue(0)
            t.join()

            self.assertEqual(results, [0], "the MoveAxis must be rejected")
            self.assertEqual(h.lib.host_guiding(), 1,
                             "a rejected MoveAxis must not cancel the active PulseGuide")
            self.assertEqual(h.lib.host_tracking(), TRACKING_SIDEREAL)
        finally:
            h.lib.host_set_pause_before_enqueue(0)
            t.join()
            h.reset()

    def test_accepted_move_axis_transitions_from_tracking(self):
        h = self.h
        self.assertEqual(h.lib.host_mount_tracking(TRACKING_SIDEREAL), 1)
        h.wait_status(STATUS_TRACKING)

        # An accepted MoveAxis from tracking transitions to manual motion.
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.5, 0.0), 1)
        h.wait_status(STATUS_SLEWING)
        self.assertEqual(h.lib.host_manual_move(), 1,
                         "the accepted MoveAxis must open a manual session")
        self.assertEqual(h.lib.host_saved_tracking(), TRACKING_SIDEREAL,
                         "the tracking mode must be saved for restore")

        h.wait_until(lambda: h.lib.host_step_count() > 0, 2.0,
                     "the manual move must emit pulses")

        # Ending the session restores the saved tracking.
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.0, 0.0), 1)
        h.wait_status(STATUS_TRACKING)
        self.assertEqual(h.lib.host_tracking(), TRACKING_SIDEREAL,
                         "ending the session must restore the saved tracking")
        self.assertEqual(h.lib.host_manual_move(), 0)

    def test_rejected_move_axis_update_preserves_existing_session(self):
        h = self.h
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.5, 0.0), 1)
        h.wait_status(STATUS_SLEWING)
        self.assertEqual(h.lib.host_manual_move(), 1)

        ra_before = h.lib.host_move_axis_ra_dps()
        dec_before = h.lib.host_move_axis_dec_dps()
        ver_before = h.lib.host_move_axis_version()
        saved_before = h.lib.host_saved_tracking()

        # Fill the queue: the MoveAxis slewing loop does not drain TRACK inline.
        fill = [h.lib.host_mount_tracking(TRACKING_SIDEREAL) for _ in range(10)]
        self.assertEqual(fill, [1] * 10, "all ten fill commands must enqueue")

        self.assertEqual(h.lib.host_mount_move_axis_speed(0.3, 0.0), 0,
                         "the MoveAxis update must be rejected")
        self.assertEqual(h.lib.host_manual_move(), 1,
                         "the existing session must remain valid")
        self.assertEqual(h.lib.host_move_axis_ra_dps(), ra_before,
                         "the setpoint must not change")
        self.assertEqual(h.lib.host_move_axis_dec_dps(), dec_before)
        self.assertEqual(h.lib.host_move_axis_version(), ver_before,
                         "the version must not change")
        self.assertEqual(h.lib.host_saved_tracking(), saved_before,
                         "the saved tracking must not change")

        self.assertEqual(h.lib.host_mount_stop(), 1)


if __name__ == "__main__":
    unittest.main()
