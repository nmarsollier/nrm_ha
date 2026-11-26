"""Host tests — MoveAxis admission and manual-session publication are atomic.

PR5/E01: accepting a MoveAxis (enqueue + setpoint publication) and publishing the
existence of the manual session were two independent operations under different
locks.  A STOP landing between them invalidated the command but the producer then
re-wrote ``manual_move = true``, leaving a phantom manual session.

These tests drive the real production admission, MoveAxis and STOP paths through
the weak ``motors_test_hook_after_move_axis_enqueue`` hook to pause the producer
deterministically at the edge of the publication window — no sleeps are used to
order the operations, only bounded waits to detect a deadlock or a harness
failure.
"""
import threading
import unittest

from harness import (Harness, STATUS_READY, STATUS_SLEWING,
                     TRACKING_NONE, EAST)


class MoveAxisAdmissionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.h = Harness()

    def setUp(self):
        self.h.reset()

    # ── Test 1 — STOP between enqueue and manual-session publication ────────

    def test_stop_after_move_axis_enqueue_does_not_resurrect_session(self):
        h = self.h
        # Preparation: READY, no manual session, zero manual setpoints.
        self.assertEqual(h.lib.host_status(), STATUS_READY)
        self.assertEqual(h.lib.host_manual_move(), 0)
        self.assertEqual(h.lib.host_move_axis_ra_dps(), 0.0)
        self.assertEqual(h.lib.host_move_axis_dec_dps(), 0.0)

        h.lib.host_set_pause_after_enqueue(1)
        h.lib.host_clear_enqueue_hook()

        results = []

        def producer():
            results.append(h.lib.host_mount_move_axis_speed(0.5, 0.0))

        t = threading.Thread(target=producer)
        t.start()

        h.wait_until(lambda: h.lib.host_reached_enqueue_hook() == 1,
                     2.0, "producer reached the after-enqueue hook")

        # STOP while the producer is suspended between the enqueue and the
        # manual-session publication.
        self.assertEqual(h.lib.host_mount_stop(), 1, "STOP must succeed")
        self.assertEqual(h.lib.host_status(), STATUS_READY)
        self.assertEqual(h.lib.host_manual_move(), 0,
                         "STOP must invalidate the manual session")
        steps_after_stop = h.lib.host_step_count()

        # Resume the producer; it must not resurrect the invalidated session.
        h.lib.host_set_pause_after_enqueue(0)
        t.join()
        # MoveAxis may still report success: it was legitimately accepted before
        # STOP.  The important thing is that STOP invalidates it definitively.
        self.assertEqual(results, [1])

        self.assertEqual(h.lib.host_manual_move(), 0,
                         "resuming the producer must not resurrect the session")
        self.assertEqual(h.lib.host_status(), STATUS_READY)
        self.assertEqual(h.lib.host_tracking(), TRACKING_NONE)
        self.assertEqual(h.lib.host_saved_tracking(), TRACKING_NONE,
                         "no tracking restore may survive a session STOP killed")
        self.assertEqual(h.lib.host_step_count(), steps_after_stop,
                         "the invalidated MoveAxis must not emit new pulses")

    # ── Test 2 — zero setpoint after accepting MoveAxis ─────────────────────

    def test_zero_after_move_axis_enqueue_cancels_pending_session(self):
        h = self.h
        self.assertEqual(h.lib.host_status(), STATUS_READY)
        self.assertEqual(h.lib.host_manual_move(), 0)
        self.assertEqual(h.lib.host_tracking(), TRACKING_NONE)

        h.lib.host_set_pause_after_enqueue(1)
        h.lib.host_clear_enqueue_hook()

        results = []

        def producer():
            results.append(h.lib.host_mount_move_axis_speed(0.5, 0.0))

        t = threading.Thread(target=producer)
        t.start()
        h.wait_until(lambda: h.lib.host_reached_enqueue_hook() == 1,
                     2.0, "producer reached the after-enqueue hook")

        # A zero while the producer is suspended must recognise the accepted
        # session and cancel it — not be dismissed as a redundant zero.
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.0, 0.0), 1)
        steps_after_zero = h.lib.host_step_count()

        h.lib.host_set_pause_after_enqueue(0)
        t.join()
        self.assertEqual(results, [1])

        self.assertEqual(h.lib.host_manual_move(), 0,
                         "the manual session must not reappear after the zero")
        h.wait_status(STATUS_READY)
        self.assertEqual(h.lib.host_status(), STATUS_READY)
        self.assertEqual(h.lib.host_step_count(), steps_after_zero,
                         "the cancelled command must not emit pulses afterwards")
        self.assertEqual(h.lib.host_tracking(), TRACKING_NONE)

    # ── Test 3 — a rejected enqueue must not create a manual session ────────

    def test_rejected_move_axis_does_not_create_manual_session(self):
        h = self.h
        self.assertEqual(h.lib.host_status(), STATUS_READY)
        self.assertEqual(h.lib.host_manual_move(), 0)
        self.assertEqual(h.lib.host_move_axis_ra_dps(), 0.0)
        self.assertEqual(h.lib.host_move_axis_dec_dps(), 0.0)
        self.assertEqual(h.lib.host_move_axis_version(), 0)

        # Hold the executor after it dequeues the first command, then fill the
        # queue to capacity with real PulseGuide orders (which do not change the
        # mount status nor create a manual session).
        h.lib.host_set_pause_after_process_command(1)
        h.lib.host_clear_process_command_hook()

        try:
            self.assertEqual(h.lib.host_mount_guide(EAST, 200), 1)
            h.wait_until(lambda: h.lib.host_reached_process_command_hook() == 1,
                         2.0, "executor reached the process-command hook")

            # The queue is now empty (the first guide was dequeued); fill it.
            fill = [h.lib.host_mount_guide(EAST, 200) for _ in range(10)]
            self.assertEqual(fill, [1] * 10, "all ten fill guides must enqueue")

            # The 11th order is a MoveAxis with no remaining capacity.
            self.assertEqual(h.lib.host_mount_move_axis_speed(0.3, 0.0), 0,
                             "a full queue must reject the MoveAxis")

            # The rejection must leave the session and setpoint untouched.
            self.assertEqual(h.lib.host_manual_move(), 0,
                             "a rejected MoveAxis must not create a session")
            self.assertEqual(h.lib.host_move_axis_ra_dps(), 0.0,
                             "the rejected order must not alter the setpoint")
            self.assertEqual(h.lib.host_move_axis_dec_dps(), 0.0)
            self.assertEqual(h.lib.host_move_axis_version(), 0,
                             "the setpoint version must not change on rejection")
            self.assertEqual(h.lib.host_step_count(), 0,
                             "the rejected order must not emit pulses")
        finally:
            h.lib.host_set_pause_after_process_command(0)
            h.reset()

    # ── Test 4 — a MoveAxis accepted after STOP starts a valid session ──────

    def test_move_axis_accepted_after_stop_creates_valid_session(self):
        h = self.h
        # STOP first: it must complete cleanly and reopen admission.
        self.assertEqual(h.lib.host_mount_stop(), 1)
        self.assertEqual(h.lib.host_status(), STATUS_READY)
        self.assertEqual(h.lib.host_manual_move(), 0)

        # A fresh MoveAxis after STOP is admitted and opens a real session.
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.5, 0.0), 1,
                         "admission must reopen after STOP")
        h.wait_status(STATUS_SLEWING)
        self.assertEqual(h.lib.host_manual_move(), 1,
                         "the new MoveAxis must open a manual session")
        h.wait_until(lambda: h.lib.host_step_count() > 0, 2.0,
                     "the new MoveAxis must emit pulses")

        # The new session ends cleanly.
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.0, 0.0), 1)
        h.wait_status(STATUS_READY)
        self.assertEqual(h.lib.host_manual_move(), 0,
                         "ending the session must clear manual_move")

    # ── Test 5 — a previous finalization must not clear a new session ───────

    def test_previous_session_finalization_does_not_clear_new_session(self):
        h = self.h
        # Session A: a valid manual move.
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.5, 0.0), 1)
        h.wait_status(STATUS_SLEWING)
        self.assertEqual(h.lib.host_manual_move(), 1)

        # Arm the hook that pauses the finalization just after it reopens
        # admission, so a fresh MoveAxis can be admitted in that window.
        h.lib.host_set_pause_after_move_axis_stop_open(1)
        h.lib.host_clear_move_axis_stop_open_hook()

        stop_results = []

        def finalize():
            stop_results.append(h.lib.host_mount_move_axis_speed(0.0, 0.0))

        t = threading.Thread(target=finalize)
        t.start()
        h.wait_until(lambda: h.lib.host_reached_move_axis_stop_open_hook() == 1,
                     2.0, "finalization reached the post-reopen hook")

        # Session B: admitted while the old finalization is suspended.
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.3, 0.0), 1)
        self.assertEqual(h.lib.host_manual_move(), 1,
                         "the new session must be active immediately")

        # Let the old finalization finish; it must not clear session B.
        h.lib.host_set_pause_after_move_axis_stop_open(0)
        t.join()
        self.assertEqual(stop_results, [1], "the finalization must report success")

        self.assertEqual(h.lib.host_manual_move(), 1,
                         "the previous finalization must not clear the new session")

        # Clean up session B.
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.0, 0.0), 1)
        h.wait_status(STATUS_READY)
        self.assertEqual(h.lib.host_manual_move(), 0)


if __name__ == "__main__":
    unittest.main()
