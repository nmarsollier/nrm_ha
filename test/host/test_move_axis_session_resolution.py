"""Host tests — PR5/E04: session-identity resolution of manual finalization.

These tests close the residual E04-G race and the E04-E/F/H session-identity
gaps: the manual finalization must be confirmed only by the executor's explicit
per-session completion, never by an unrelated idle barrier, a setpoint-version
preemption, or a stale command from an older session.

Each test drives the real production paths through the weak
``motors_test_hook_*`` hooks so the race is ordered deterministically — sleeps
are used only for bounded deadlock/timeout detection, never to order the race.
"""
import threading
import unittest

from harness import (Harness, STATUS_READY, STATUS_SLEWING, STATUS_TRACKING,
                     TRACKING_SIDEREAL, TRACKING_NONE)


class MoveAxisSessionResolutionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.h = Harness()

    def setUp(self):
        self.h.reset()

    # ── Test A: a setpoint update must not confirm the finalization ─────────

    def test_setpoint_preemption_does_not_acknowledge_manual_finalization(self):
        h = self.h
        # Hold the executor right after it processes the first MoveAxis command
        # (before its motion loop), so the reverse command stays queued while the
        # session is finalised.
        h.lib.host_set_pause_after_process_command(1)
        h.lib.host_clear_process_command_hook()

        self.assertEqual(h.lib.host_mount_move_axis_speed(1.0, 0.0), 1)
        h.wait_until(lambda: h.lib.host_reached_process_command_hook() == 1,
                     2.0, "executor processed the forward command")

        # A reverse rate in the same session: a setpoint-version bump, not a
        # completion.  The reverse command stays queued (the executor is held).
        self.assertEqual(h.lib.host_mount_move_axis_speed(-1.0, 0.0), 1)

        # Finalize the session while the reverse command is still queued.
        results = []

        def finalize():
            results.append(h.lib.host_mount_move_axis_speed(0.0, 0.0))

        t = threading.Thread(target=finalize)
        t.start()
        h.wait_until(lambda: h.lib.host_manual_move() == 0, 2.0,
                     "session invalidated by finalization")

        # Release the executor: it must reconcile the preempted forward motion
        # and the discarded reverse command before the finalization returns.
        h.lib.host_set_pause_after_process_command(0)
        t.join(timeout=5.0)

        self.assertEqual(results, [1], "finalization must report success")
        # The mount must not be left stuck SLEWING after a successful return.
        h.wait_status(STATUS_READY)

    # ── Test B: a confirmed completion must not be erased ───────────────────

    def test_manual_completion_cannot_be_erased_by_requester(self):
        h = self.h
        # Successful finalization publishes the session completion.
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.5, 0.0), 1)
        h.wait_status(STATUS_SLEWING)
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.0, 0.0), 1)
        h.wait_status(STATUS_READY)
        finished_first = h.lib.host_finished_manual_session()

        # An unrelated motion (tracking) and its STOP must not erase or regress
        # the completion record.
        self.assertEqual(h.lib.host_mount_tracking(TRACKING_SIDEREAL), 1)
        h.wait_status(STATUS_TRACKING)
        self.assertEqual(h.lib.host_mount_stop(), 1)
        h.wait_status(STATUS_READY)
        self.assertEqual(h.lib.host_finished_manual_session(), finished_first,
                         "unrelated motion must not erase the completion")

        # A new session's completion advances (never regresses) the record.
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.3, 0.0), 1)
        h.wait_status(STATUS_SLEWING)
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.0, 0.0), 1)
        h.wait_status(STATUS_READY)
        self.assertGreater(h.lib.host_finished_manual_session(), finished_first,
                           "the new session's completion must advance the record")
        self.assertEqual(h.lib.host_finished_manual_aborted(), 0)

    # ── Test B (failed): a failed drain is reported and stays associated ────

    def test_failed_manual_drain_reports_error(self):
        h = self.h
        # Absent RA completion from the start: the forward motion's batches never
        # complete, so the graceful drain times out and must be reported as a
        # failure — never as a clean success.
        h.lib.host_rmt_set_absent(0)

        self.assertEqual(h.lib.host_mount_move_axis_speed(0.5, 0.0), 1)
        h.wait_status(STATUS_SLEWING)

        self.assertEqual(h.lib.host_mount_move_axis_speed(0.0, 0.0), 0,
                         "a failed drain must be reported as an error")
        self.assertEqual(h.lib.host_finished_manual_aborted(), 1,
                         "the failed drain's result must be recorded")

        # Cleanup for the next test (reset also clears the injected fault).
        h.lib.host_rmt_clear_faults()
        h.reset()

    # ── Test B (identity): a later session cannot replace a pending result ───

    def test_manual_completion_result_cannot_be_replaced_before_consumption(self):
        h = self.h
        # Session A, with RA completion absent from the start so its batches never
        # complete and its drain deterministically fails.  A's finalization holds
        # the control mutex across the drain, so any later finalization is
        # serialized behind it.
        h.lib.host_rmt_set_absent(0)
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.5, 0.0), 1)
        h.wait_status(STATUS_SLEWING)

        a_results = []

        def finalize_a():
            a_results.append(h.lib.host_mount_move_axis_speed(0.0, 0.0))

        t_a = threading.Thread(target=finalize_a)
        t_a.start()

        # Wait until A's finalization invalidated its session: it now holds the
        # control mutex and is draining (the drain will fail).
        h.wait_until(lambda: h.lib.host_manual_move() == 0, 2.0,
                     "session A invalidated")

        # Admit session B while A is still draining (admission is not closed by
        # a manual finalization).
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.3, 0.0), 1)
        self.assertEqual(h.lib.host_manual_move(), 1, "session B must be active")

        # Restore RA completion so B's later drain is clean.  A's in-flight
        # batches already lost their completions, so its drain still times out.
        h.lib.host_rmt_clear_faults()

        # Finalize B in a second thread: it must block on the control mutex
        # until A's finalization returns — demonstrating the serialization that
        # keeps A's result from being replaced before it is consumed.
        b_results = []

        def finalize_b():
            b_results.append(h.lib.host_mount_move_axis_speed(0.0, 0.0))

        t_b = threading.Thread(target=finalize_b)
        t_b.start()

        # A's finalization completes first and reports its OWN failure (not B's
        # clean result, which could not have been published yet).
        t_a.join(timeout=5.0)
        self.assertEqual(a_results, [0],
                         "A must report its own failed drain, not B's result")

        # E06: A's failed drain closed admission at the point of detection, so
        # session B (admitted before the failure) is invalidated.  B's
        # finalization is then a redundant zero — it must not overwrite A's
        # failure or restore a position that is now uncertain.
        t_b.join(timeout=5.0)
        self.assertEqual(b_results, [1], "B's finalization is a redundant zero")

        self.assertEqual(h.lib.host_position_uncertain(), 1,
                         "A's failed drain must leave the position uncertain")


    def test_unrelated_idle_barrier_does_not_confirm_manual_drain(self):
        h = self.h
        before = h.lib.host_finished_manual_session()

        # A tracking motion and its STOP each run idle barriers; neither is a
        # manual-session drain, so the completion record must not advance.
        self.assertEqual(h.lib.host_mount_tracking(TRACKING_SIDEREAL), 1)
        h.wait_status(STATUS_TRACKING)
        self.assertEqual(h.lib.host_mount_stop(), 1)
        h.wait_status(STATUS_READY)

        self.assertEqual(h.lib.host_finished_manual_session(), before,
                         "an unrelated idle barrier must not confirm a drain")

    # ── Test D: an old session's command cannot run as a new session ────────

    def test_old_manual_command_cannot_execute_as_new_session(self):
        h = self.h
        # Hold the executor with a tracking sentinel so the first MoveAxis
        # command stays queued while its session is finalised and replaced.
        # (A standalone guide sentinel would leave `guiding` stale on the
        # tracking→manual transition, which is a separate defect.)
        h.lib.host_set_pause_after_process_command(1)
        h.lib.host_clear_process_command_hook()

        self.assertEqual(h.lib.host_mount_tracking(TRACKING_SIDEREAL), 1)
        h.wait_until(lambda: h.lib.host_reached_process_command_hook() == 1,
                     2.0, "executor paused on the tracking sentinel")

        # Old session S: forward rate, queued but never dequeued.
        self.assertEqual(h.lib.host_mount_move_axis_speed(1.0, 0.0), 1)

        results = []

        def finalize():
            results.append(h.lib.host_mount_move_axis_speed(0.0, 0.0))

        t = threading.Thread(target=finalize)
        t.start()
        h.wait_until(lambda: h.lib.host_manual_move() == 0, 2.0,
                     "session S invalidated by finalization")

        # New session S+1 admitted while the old command is still queued.
        self.assertEqual(h.lib.host_mount_move_axis_speed(-0.5, 0.0), 1)
        self.assertEqual(h.lib.host_manual_move(), 1, "new session must be active")

        h.lib.host_set_pause_after_process_command(0)
        t.join(timeout=5.0)

        # The old finalization succeeds only because the old command was
        # discarded — had it executed, the finalization would have timed out.
        self.assertEqual(results, [1], "the old session must be finalised")
        h.wait_status(STATUS_SLEWING)
        self.assertEqual(h.lib.host_manual_move(), 1,
                         "the new session must be running")

        # Cleanup: the new session restores the sentinel's tracking, then STOP.
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.0, 0.0), 1)
        h.wait_status(STATUS_TRACKING)
        self.assertEqual(h.lib.host_mount_stop(), 1)

    # ── Test E: finalization does not stop an unrelated motion ──────────────

    def test_manual_finalization_cannot_stop_unrelated_motion(self):
        h = self.h
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.5, 0.0), 1)
        h.wait_status(STATUS_SLEWING)

        h.lib.host_set_pause_after_move_axis_stop_open(1)
        h.lib.host_clear_move_axis_stop_open_hook()

        results = []

        def finalize():
            results.append(h.lib.host_mount_move_axis_speed(0.0, 0.0))

        t = threading.Thread(target=finalize)
        t.start()
        h.wait_until(lambda: h.lib.host_reached_move_axis_stop_open_hook() == 1,
                     2.0, "finalization reached the post-drain hook")

        # An unrelated tracking admitted while the finalization is suspended.
        self.assertEqual(h.lib.host_mount_tracking(TRACKING_SIDEREAL), 1)

        h.lib.host_set_pause_after_move_axis_stop_open(0)
        t.join(timeout=5.0)
        self.assertEqual(results, [1], "finalization must report success")

        # The unrelated tracking must survive and run.
        h.wait_status(STATUS_TRACKING)
        self.assertEqual(h.lib.host_tracking(), TRACKING_SIDEREAL)

        self.assertEqual(h.lib.host_mount_stop(), 1)

    # ── Test F: finalization does not publish READY over a new slew ─────────

    def test_manual_finalization_does_not_publish_ready_over_new_slew(self):
        h = self.h
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.5, 0.0), 1)
        h.wait_status(STATUS_SLEWING)

        h.lib.host_set_pause_after_move_axis_stop_open(1)
        h.lib.host_clear_move_axis_stop_open_hook()

        results = []

        def finalize():
            results.append(h.lib.host_mount_move_axis_speed(0.0, 0.0))

        t = threading.Thread(target=finalize)
        t.start()
        h.wait_until(lambda: h.lib.host_reached_move_axis_stop_open_hook() == 1,
                     2.0, "finalization reached the post-drain hook")

        # A fresh manual session (a new SLEW) admitted while the old finalization
        # is suspended after its drain.
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.3, 0.0), 1)
        self.assertEqual(h.lib.host_manual_move(), 1)

        h.lib.host_set_pause_after_move_axis_stop_open(0)
        t.join(timeout=5.0)
        self.assertEqual(results, [1], "finalization must report success")

        # The new session stays SLEWING: the old finalization's READY must not
        # have overwritten it.
        h.wait_status(STATUS_SLEWING)
        self.assertEqual(h.lib.host_manual_move(), 1)

        self.assertEqual(h.lib.host_mount_move_axis_speed(0.0, 0.0), 1)
        h.wait_status(STATUS_READY)

    # ── Test G: an old executor cannot save tracking into a new session ─────

    def test_old_executor_cannot_save_tracking_into_new_manual_session(self):
        h = self.h
        self.assertEqual(h.lib.host_mount_tracking(TRACKING_SIDEREAL), 1)
        h.wait_status(STATUS_TRACKING)

        h.lib.host_set_pause_before_move_axis_saved_tracking_publish(1)
        h.lib.host_clear_before_move_axis_saved_tracking_publish_hook()

        # The executor captures the tracking mode and pauses before publishing
        # saved_tracking (session S).
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.5, 0.0), 1)
        h.wait_until(
            lambda: h.lib.host_reached_before_move_axis_saved_tracking_publish_hook() == 1,
            2.0, "executor reached the saved-tracking-publish hook")

        # Finalize session S (in a thread — it waits for the executor to drain).
        results = []

        def finalize():
            results.append(h.lib.host_mount_move_axis_speed(0.0, 0.0))

        t = threading.Thread(target=finalize)
        t.start()
        h.wait_until(lambda: h.lib.host_manual_move() == 0, 2.0,
                     "session S invalidated by finalization")

        # New session S+1, admitted while the executor is still paused.
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.3, 0.0), 1)
        self.assertEqual(h.lib.host_manual_move(), 1)

        # Release the executor: session S's saved tracking must not leak into
        # session S+1 (the session identity differs).
        h.lib.host_set_pause_before_move_axis_saved_tracking_publish(0)
        t.join(timeout=5.0)
        self.assertEqual(results, [1], "finalization must report success")

        self.assertEqual(h.lib.host_saved_tracking(), TRACKING_NONE,
                         "the old executor must not save tracking into the new session")

        self.assertEqual(h.lib.host_mount_move_axis_speed(0.0, 0.0), 1)
        h.wait_status(STATUS_READY)


if __name__ == "__main__":
    unittest.main()
