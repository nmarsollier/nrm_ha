"""Host tests — PR5/E03: stale tracking capture, late producer write, and stale
tracking restore around the MoveAxis manual session.

E03-A: the producer captured the tracking mode before admission, so a STOP or a
mode change in between left the session saving a stale mode.
E03-B: after admission the producer wrote `motors_state.tracking = TRACKING_NONE`
outside the admission transaction, so an invalidated producer could still cancel
a tracking operation started afterwards.
E03-C: the manual-session finalization restored the saved tracking after
reopening admission, so it could race a new session.

These tests drive the real production paths through the weak hooks.  The
atomics-backed hooks pause the producer/motion task deterministically; sleeps are
used only for bounded deadlock/timeout detection, never to order the race.
"""
import threading
import unittest

from harness import (Harness, STATUS_READY, STATUS_SLEWING, STATUS_TRACKING,
                     STATUS_PARKED, TRACKING_SIDEREAL, TRACKING_LUNAR,
                     TRACKING_NONE)

MOTION_CMD_TRACK = 1  # MotionCommandType.MOTION_CMD_TRACK


class MoveAxisTrackingRestoreTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.h = Harness()

    def setUp(self):
        self.h.reset()

    # ── E03-A: stale tracking capture before admission ─────────────────────

    def test_move_axis_does_not_save_tracking_cancelled_before_admission(self):
        h = self.h
        self.assertEqual(h.lib.host_mount_tracking(TRACKING_SIDEREAL), 1)
        h.wait_status(STATUS_TRACKING)

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

            # STOP cancels the tracking while the producer is suspended.
            self.assertEqual(h.lib.host_mount_stop(), 1)
            self.assertEqual(h.lib.host_tracking(), TRACKING_NONE)

            # Resume: MoveAxis is admitted in the new generation.  The tracking
            # captured before STOP must NOT be recorded for this new session.
            h.lib.host_set_pause_before_enqueue(0)
            t.join()
            self.assertEqual(results, [1], "the MoveAxis must be admitted")

            self.assertEqual(h.lib.host_saved_tracking(), TRACKING_NONE,
                             "a session admitted after STOP must not save the "
                             "tracking STOP cancelled")
            # Ending the session must not reactivate the cancelled tracking.
            self.assertEqual(h.lib.host_mount_move_axis_speed(0.0, 0.0), 1)
            h.wait_status(STATUS_READY)
            self.assertEqual(h.lib.host_tracking(), TRACKING_NONE,
                             "finalization must not restore tracking STOP cancelled")
        finally:
            h.lib.host_set_pause_before_enqueue(0)
            t.join()
            h.reset()

    def test_move_axis_saves_current_tracking_mode_at_admission(self):
        h = self.h
        self.assertEqual(h.lib.host_mount_tracking(TRACKING_SIDEREAL), 1)
        h.wait_status(STATUS_TRACKING)

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

            # Legitimately change the tracking mode while the producer is held.
            self.assertEqual(h.lib.host_mount_tracking(TRACKING_LUNAR), 1)
            h.wait_until(lambda: h.lib.host_tracking() == TRACKING_LUNAR, 2.0,
                         "tracking switched to lunar")

            h.lib.host_set_pause_before_enqueue(0)
            t.join()
            self.assertEqual(results, [1], "the MoveAxis must be admitted")

            # Wait for the executor to process the transition and capture the mode.
            h.wait_status(STATUS_SLEWING)

            # The session must record the mode active at the transition, not the
            # stale sidereal captured before the mode change.
            self.assertEqual(h.lib.host_saved_tracking(), TRACKING_LUNAR,
                             "the session must save the mode active at admission")
        finally:
            h.lib.host_set_pause_before_enqueue(0)
            t.join()
            h.reset()

    # ── E03-B: late producer write cannot cancel new tracking ──────────────

    def test_invalidated_move_axis_producer_does_not_stop_new_tracking(self):
        h = self.h
        self.assertEqual(h.lib.host_mount_tracking(TRACKING_SIDEREAL), 1)
        h.wait_status(STATUS_TRACKING)

        h.lib.host_set_pause_before_move_axis_tracking_transition(1)
        h.lib.host_clear_before_move_axis_tracking_transition_hook()

        results = []

        def producer():
            results.append(h.lib.host_mount_move_axis_speed(0.5, 0.0))

        t = threading.Thread(target=producer)
        t.start()
        try:
            h.wait_until(lambda: h.lib.host_reached_before_move_axis_tracking_transition_hook() == 1,
                         2.0, "producer reached the post-admission hook")

            # STOP invalidates the just-admitted session.
            self.assertEqual(h.lib.host_mount_stop(), 1)

            # A new, valid tracking operation starts.
            self.assertEqual(h.lib.host_mount_tracking(TRACKING_SIDEREAL), 1)
            h.wait_status(STATUS_TRACKING)
            self.assertEqual(h.lib.host_tracking(), TRACKING_SIDEREAL)

            # Resume the invalidated producer; it must not cancel the new tracking.
            h.lib.host_set_pause_before_move_axis_tracking_transition(0)
            t.join()
            self.assertEqual(results, [1], "the MoveAxis was admitted before STOP")

            self.assertEqual(h.lib.host_tracking(), TRACKING_SIDEREAL,
                             "an invalidated producer must not stop the new tracking")
            self.assertEqual(h.lib.host_status(), STATUS_TRACKING)
        finally:
            h.lib.host_set_pause_before_move_axis_tracking_transition(0)
            t.join()
            h.reset()

    # ── E03-C: restore cannot survive STOP / PARK / a new session ─────────

    def test_manual_tracking_restore_cannot_survive_later_stop(self):
        h = self.h
        self.assertEqual(h.lib.host_mount_tracking(TRACKING_SIDEREAL), 1)
        h.wait_status(STATUS_TRACKING)
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.5, 0.0), 1)
        h.wait_status(STATUS_SLEWING)
        self.assertEqual(h.lib.host_saved_tracking(), TRACKING_SIDEREAL)

        # A later STOP must not be followed by any restore of the cancelled mode.
        self.assertEqual(h.lib.host_mount_stop(), 1)
        self.assertEqual(h.lib.host_status(), STATUS_READY)
        self.assertEqual(h.lib.host_tracking(), TRACKING_NONE)
        self.assertEqual(h.lib.host_saved_tracking(), TRACKING_NONE)

        # A redundant zero after the STOP must not reactivate anything.
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.0, 0.0), 1)
        self.assertEqual(h.lib.host_tracking(), TRACKING_NONE,
                         "no tracking restore may survive a later STOP")

    def test_manual_tracking_restore_cannot_survive_park(self):
        h = self.h
        self.assertEqual(h.lib.host_mount_tracking(TRACKING_SIDEREAL), 1)
        h.wait_status(STATUS_TRACKING)
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.5, 0.0), 1)
        h.wait_status(STATUS_SLEWING)
        self.assertEqual(h.lib.host_saved_tracking(), TRACKING_SIDEREAL)

        self.assertEqual(h.lib.host_mount_park(), 1)
        self.assertEqual(h.lib.host_status(), STATUS_PARKED)
        self.assertEqual(h.lib.host_tracking(), TRACKING_NONE)
        self.assertEqual(h.lib.host_saved_tracking(), TRACKING_NONE,
                         "PARK must invalidate the saved tracking")

    def test_new_manual_session_does_not_inherit_previous_tracking_restore(self):
        h = self.h
        self.assertEqual(h.lib.host_mount_tracking(TRACKING_SIDEREAL), 1)
        h.wait_status(STATUS_TRACKING)
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.5, 0.0), 1)
        h.wait_status(STATUS_SLEWING)
        self.assertEqual(h.lib.host_saved_tracking(), TRACKING_SIDEREAL)

        h.lib.host_set_pause_after_move_axis_stop_open(1)
        h.lib.host_clear_move_axis_stop_open_hook()

        stop_results = []

        def finalize():
            stop_results.append(h.lib.host_mount_move_axis_speed(0.0, 0.0))

        t = threading.Thread(target=finalize)
        t.start()
        try:
            h.wait_until(lambda: h.lib.host_reached_move_axis_stop_open_hook() == 1,
                         2.0, "finalization reached the post-reopen hook")

            # A new session is admitted while the old finalization is suspended.
            self.assertEqual(h.lib.host_mount_move_axis_speed(0.3, 0.0), 1)
            self.assertEqual(h.lib.host_manual_move(), 1)

            # Resume the old finalization (it would restore sidereal tracking).
            h.lib.host_set_pause_after_move_axis_stop_open(0)
            t.join()
            self.assertEqual(stop_results, [1])

            # The new session must remain valid and unmodified.
            self.assertEqual(h.lib.host_manual_move(), 1,
                             "the old finalization must not clear the new session")
            self.assertEqual(h.lib.host_saved_tracking(), TRACKING_NONE,
                             "the new session must not inherit the old restore")
        finally:
            h.lib.host_set_pause_after_move_axis_stop_open(0)
            t.join()
            h.reset()

    # ── regression: the normal restore path ────────────────────────────────

    def test_manual_session_restores_its_original_tracking_mode(self):
        h = self.h
        # From tracking: updates must not overwrite the saved mode.
        self.assertEqual(h.lib.host_mount_tracking(TRACKING_SIDEREAL), 1)
        h.wait_status(STATUS_TRACKING)
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.5, 0.0), 1)
        h.wait_status(STATUS_SLEWING)
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.3, 0.0), 1)
        self.assertEqual(h.lib.host_saved_tracking(), TRACKING_SIDEREAL,
                         "updates must not overwrite the saved mode")
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.0, 0.0), 1)
        h.wait_status(STATUS_TRACKING)
        self.assertEqual(h.lib.host_tracking(), TRACKING_SIDEREAL,
                         "finalization must restore the original mode")

        self.assertEqual(h.lib.host_mount_stop(), 1)
        h.wait_status(STATUS_READY)

        # From READY: no tracking is activated on finalization.
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.5, 0.0), 1)
        h.wait_status(STATUS_SLEWING)
        self.assertEqual(h.lib.host_saved_tracking(), TRACKING_NONE)
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.0, 0.0), 1)
        h.wait_status(STATUS_READY)
        self.assertEqual(h.lib.host_tracking(), TRACKING_NONE,
                         "a READY session must not activate tracking on finalization")

    # ── E03-C residual: restore cannot be enqueued after a new session ─────

    def test_restore_cannot_be_enqueued_after_new_manual_session_admission(self):
        h = self.h
        self.assertEqual(h.lib.host_mount_tracking(TRACKING_SIDEREAL), 1)
        h.wait_status(STATUS_TRACKING)
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.5, 0.0), 1)
        h.wait_status(STATUS_SLEWING)
        self.assertEqual(h.lib.host_saved_tracking(), TRACKING_SIDEREAL)

        h.lib.host_set_pause_before_move_axis_tracking_restore(1)
        h.lib.host_clear_before_move_axis_tracking_restore_hook()

        results = []

        def finalize():
            results.append(h.lib.host_mount_move_axis_speed(0.0, 0.0))

        t = threading.Thread(target=finalize)
        t.start()
        try:
            h.wait_until(lambda: h.lib.host_reached_before_move_axis_tracking_restore_hook() == 1,
                         2.0, "finalization reached the pre-restore hook")

            # Admit a new session while the finalization is suspended before the
            # restore (its earlier manual_move re-check already decided "none").
            self.assertEqual(h.lib.host_mount_move_axis_speed(0.3, 0.0), 1)
            self.assertEqual(h.lib.host_manual_move(), 1)

            # Resume the finalization; the RED case would enqueue a stale TRACK.
            h.lib.host_set_pause_before_move_axis_tracking_restore(0)
            t.join()
            self.assertEqual(results, [1], "the finalization must report success")

            # The new session must stay active and no stale TRACK may be queued.
            h.wait_status(STATUS_SLEWING)
            self.assertEqual(h.lib.host_manual_move(), 1,
                             "the new session must remain valid")
            self.assertNotEqual(h.lib.host_queue_front_type(), MOTION_CMD_TRACK,
                                "no stale tracking restore may be queued after "
                                "the new session")
        finally:
            h.lib.host_set_pause_before_move_axis_tracking_restore(0)
            t.join()
            h.reset()

    # ── E03-D: executor cannot publish saved tracking after invalidation ───

    def test_executor_cannot_publish_saved_tracking_after_session_invalidation(self):
        h = self.h
        self.assertEqual(h.lib.host_mount_tracking(TRACKING_SIDEREAL), 1)
        h.wait_status(STATUS_TRACKING)

        h.lib.host_set_pause_before_move_axis_saved_tracking_publish(1)
        h.lib.host_clear_before_move_axis_saved_tracking_publish_hook()
        h.lib.host_set_pause_after_stop_begin(1)
        h.lib.host_clear_stop_begin_hook()

        # Admit MoveAxis; the executor captures the tracking mode and pauses
        # before publishing saved_tracking.
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.5, 0.0), 1)
        h.wait_until(lambda: h.lib.host_reached_before_move_axis_saved_tracking_publish_hook() == 1,
                     2.0, "executor reached the saved-tracking-publish hook")

        stop_results = []

        def stopper():
            stop_results.append(h.lib.host_mount_stop())

        t = threading.Thread(target=stopper)
        t.start()

        # STOP closes admission and invalidates the session, then pauses.
        h.wait_until(lambda: h.lib.host_reached_stop_begin_hook() == 1,
                     2.0, "STOP reached the after-stop-begin hook")

        # Resume the executor (it would publish saved tracking), then STOP.
        h.lib.host_set_pause_before_move_axis_saved_tracking_publish(0)
        h.lib.host_set_pause_after_stop_begin(0)
        t.join()

        self.assertEqual(stop_results, [1], "STOP must succeed")
        self.assertEqual(h.lib.host_saved_tracking(), TRACKING_NONE,
                         "the executor must not publish saved tracking after "
                         "the session was invalidated")
        self.assertEqual(h.lib.host_manual_move(), 0,
                         "the session must remain invalidated")

    # ── concurrent finalization vs STOP: no restore survives ──────────────

    def test_concurrent_finalization_and_stop_do_not_restore_tracking(self):
        h = self.h
        self.assertEqual(h.lib.host_mount_tracking(TRACKING_SIDEREAL), 1)
        h.wait_status(STATUS_TRACKING)
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.5, 0.0), 1)
        h.wait_status(STATUS_SLEWING)
        self.assertEqual(h.lib.host_saved_tracking(), TRACKING_SIDEREAL)

        results = []

        def finalize():
            results.append(h.lib.host_mount_move_axis_speed(0.0, 0.0))

        def stopper():
            results.append(h.lib.host_mount_stop())

        t1 = threading.Thread(target=finalize)
        t2 = threading.Thread(target=stopper)
        t1.start()
        t2.start()
        t1.join()
        t2.join()

        # Both operations complete; the control mutex serialises them so that
        # once STOP succeeds no restore from the earlier session survives.
        self.assertEqual(results, [1, 1], "both operations must succeed")
        self.assertEqual(h.lib.host_status(), STATUS_READY)
        self.assertEqual(h.lib.host_tracking(), TRACKING_NONE,
                         "no tracking restore may survive a concurrent STOP")
        self.assertEqual(h.lib.host_saved_tracking(), TRACKING_NONE)

    # ── a needed restore that cannot be admitted must report the error ─────

    def test_tracking_restore_admission_failure_is_reported(self):
        h = self.h
        self.assertEqual(h.lib.host_mount_tracking(TRACKING_SIDEREAL), 1)
        h.wait_status(STATUS_TRACKING)
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.5, 0.0), 1)
        h.wait_status(STATUS_SLEWING)
        self.assertEqual(h.lib.host_saved_tracking(), TRACKING_SIDEREAL)

        h.lib.host_set_pause_before_move_axis_tracking_restore(1)
        h.lib.host_clear_before_move_axis_tracking_restore_hook()
        h.lib.host_set_pause_after_process_command(1)
        h.lib.host_clear_process_command_hook()

        results = []

        def finalize():
            results.append(h.lib.host_mount_move_axis_speed(0.0, 0.0))

        t = threading.Thread(target=finalize)
        t.start()
        try:
            h.wait_until(lambda: h.lib.host_reached_before_move_axis_tracking_restore_hook() == 1,
                         2.0, "finalization reached the pre-restore hook")

            # Fill the queue without creating a new manual session: pause the
            # executor (it dequeues and processes one SLEW, then stops) and fill
            # the remaining capacity.
            self.assertEqual(h.lib.host_fill_queue_with_slew(1), 1)
            h.wait_until(lambda: h.lib.host_reached_process_command_hook() == 1,
                         2.0, "executor reached the process-command hook")
            self.assertEqual(h.lib.host_fill_queue_with_slew(10), 10)

            # Resume the finalization: the restore must fail to enqueue and the
            # error must be reported (not swallowed as best-effort).
            h.lib.host_set_pause_before_move_axis_tracking_restore(0)
            t.join()

            self.assertEqual(results, [0],
                             "a failed tracking-restore admission must be reported")
        finally:
            h.lib.host_set_pause_before_move_axis_tracking_restore(0)
            h.lib.host_set_pause_after_process_command(0)
            t.join()
            h.reset()


if __name__ == "__main__":
    unittest.main()
