"""Host tests — PR5/E05: atomic per-axis MoveAxis setpoint updates.

The single-axis update (`mount_set_move_axis_rate`) used to read both setpoints,
modify one and re-publish both as three separate steps, so concurrent single-axis
updates could lose each other's rate.  These tests drive the real production
path through the ``motors_test_hook_before_move_axis_enqueue`` hook (which sits
between the read and the publication in the vulnerable implementation) and the
E04 finalization hooks.

Sleeps are used only for bounded deadlock/timeout detection, never to order the
race; the hook's own pause provides the deterministic ordering.
"""
import threading
import unittest

from harness import (Harness, STATUS_READY, STATUS_SLEWING, STATUS_TRACKING,
                     TRACKING_SIDEREAL, TRACKING_NONE, EAST)


class MoveAxisConcurrentSetpointTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.h = Harness()

    def setUp(self):
        self.h.reset()

    # ── Test 1: concurrent updates of different axes preserve both ──────────

    def test_concurrent_ra_dec_updates_preserve_both_rates(self):
        h = self.h
        h.lib.host_arm_first_producer_gate(1)
        h.lib.host_clear_first_producer_gate()

        results = []

        def update_ra():
            results.append(h.lib.host_mount_move_axis_rate(0, 0.5))

        t = threading.Thread(target=update_ra)
        t.start()
        h.wait_until(lambda: h.lib.host_reached_first_producer_gate() == 1,
                     2.0, "RA producer reached the before-enqueue hook")

        # DEC updates while RA is suspended before its publication.
        self.assertEqual(h.lib.host_mount_move_axis_rate(1, 0.3), 1)
        h.lib.host_release_first_producer()

        t.join(timeout=5.0)
        self.assertEqual(results, [1], "the RA update must be accepted")

        self.assertAlmostEqual(h.lib.host_move_axis_ra_dps(), 0.5, places=6,
                         msg="RA must keep its own rate")
        self.assertAlmostEqual(h.lib.host_move_axis_dec_dps(), 0.3, places=6,
                         msg="DEC must keep its own rate")
        self.assertEqual(h.lib.host_manual_move(), 1,
                         "a single valid manual session must exist")

        self.assertEqual(h.lib.host_mount_move_axis_speed(0.0, 0.0), 1)
        h.wait_status(STATUS_READY)

    # ── Test 2: zeroing one axis does not lose the other's update ───────────

    def test_zero_rate_does_not_overwrite_concurrent_other_axis_update(self):
        h = self.h
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.5, 0.3), 1)
        h.wait_status(STATUS_SLEWING)

        h.lib.host_arm_first_producer_gate(1)
        h.lib.host_clear_first_producer_gate()

        results = []

        def zero_ra():
            results.append(h.lib.host_mount_move_axis_rate(0, 0.0))

        t = threading.Thread(target=zero_ra)
        t.start()
        h.wait_until(lambda: h.lib.host_reached_first_producer_gate() == 1,
                     2.0, "RA zero reached the before-enqueue hook")

        self.assertEqual(h.lib.host_mount_move_axis_rate(1, 0.7), 1)
        h.lib.host_release_first_producer()

        t.join(timeout=5.0)
        self.assertEqual(results, [1], "the RA zero must be accepted")

        self.assertEqual(h.lib.host_move_axis_ra_dps(), 0.0,
                         "RA must be stopped")
        self.assertAlmostEqual(h.lib.host_move_axis_dec_dps(), 0.7, places=6,
                         msg="DEC must keep its most recent rate")
        self.assertEqual(h.lib.host_manual_move(), 1,
                         "the session must remain valid (DEC still moving)")

        self.assertEqual(h.lib.host_mount_move_axis_speed(0.0, 0.0), 1)
        h.wait_status(STATUS_READY)

    # ── Test 3: a stale zero snapshot must not finalize the session ─────────

    def test_stale_zero_snapshot_cannot_finalize_active_other_axis(self):
        h = self.h
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.5, 0.0), 1)
        h.wait_status(STATUS_SLEWING)

        h.lib.host_arm_first_producer_gate(1)
        h.lib.host_clear_first_producer_gate()

        results = []

        def zero_ra():
            results.append(h.lib.host_mount_move_axis_rate(0, 0.0))

        t = threading.Thread(target=zero_ra)
        t.start()
        h.wait_until(lambda: h.lib.host_reached_first_producer_gate() == 1,
                     2.0, "RA zero reached the before-enqueue hook")

        self.assertEqual(h.lib.host_mount_move_axis_rate(1, 0.3), 1)
        h.lib.host_release_first_producer()

        t.join(timeout=5.0)
        self.assertEqual(results, [1], "the RA zero must be accepted")

        self.assertEqual(h.lib.host_move_axis_ra_dps(), 0.0)
        self.assertAlmostEqual(h.lib.host_move_axis_dec_dps(), 0.3, places=6,
                         msg="the concurrent DEC update must not be cancelled")
        self.assertEqual(h.lib.host_manual_move(), 1,
                         "the session must remain active (DEC still moving)")

        self.assertEqual(h.lib.host_mount_move_axis_speed(0.0, 0.0), 1)
        h.wait_status(STATUS_READY)

    # ── Test 4: STOP must not allow resurrection of a pre-STOP setpoint ─────

    def test_single_axis_update_cannot_resurrect_pre_stop_setpoint(self):
        h = self.h
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.5, 0.0), 1)
        h.wait_status(STATUS_SLEWING)

        h.lib.host_arm_first_producer_gate(1)
        h.lib.host_clear_first_producer_gate()

        results = []

        def update_dec():
            results.append(h.lib.host_mount_move_axis_rate(1, 0.3))

        t = threading.Thread(target=update_dec)
        t.start()
        h.wait_until(lambda: h.lib.host_reached_first_producer_gate() == 1,
                     2.0, "DEC update reached the before-enqueue hook")

        # A global STOP invalidates the session and its setpoints.
        self.assertEqual(h.lib.host_mount_stop(), 1)
        h.wait_status(STATUS_READY)

        h.lib.host_release_first_producer()
        t.join(timeout=5.0)

        # The update is a fresh session: RA must not resurrect the pre-STOP 0.5.
        self.assertEqual(results, [1], "the DEC update must be accepted")
        self.assertEqual(h.lib.host_move_axis_ra_dps(), 0.0,
                         "RA must not resurrect the pre-STOP setpoint")
        self.assertAlmostEqual(h.lib.host_move_axis_dec_dps(), 0.3, places=6,
                         msg="DEC must be applied to the fresh session")
        self.assertEqual(h.lib.host_manual_move(), 1)

        self.assertEqual(h.lib.host_mount_stop(), 1)

    # ── Test 5: concurrent axis zeros finalize the session exactly once ─────

    def test_concurrent_axis_zeros_finalize_session_exactly_once(self):
        h = self.h
        self.assertEqual(h.lib.host_mount_tracking(TRACKING_SIDEREAL), 1)
        h.wait_status(STATUS_TRACKING)
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.5, 0.3), 1)
        h.wait_status(STATUS_SLEWING)
        self.assertEqual(h.lib.host_saved_tracking(), TRACKING_SIDEREAL)
        gen_before = h.lib.host_stop_generation()

        results = []

        def zero_ra():
            results.append(h.lib.host_mount_move_axis_rate(0, 0.0))

        def zero_dec():
            results.append(h.lib.host_mount_move_axis_rate(1, 0.0))

        t1 = threading.Thread(target=zero_ra)
        t2 = threading.Thread(target=zero_dec)
        t1.start()
        t2.start()
        t1.join(timeout=5.0)
        t2.join(timeout=5.0)

        self.assertEqual(sorted(results), [1, 1],
                         "both zeros must be accepted")
        h.wait_status(STATUS_TRACKING)
        self.assertEqual(h.lib.host_tracking(), TRACKING_SIDEREAL,
                         "the saved tracking must be restored")
        self.assertEqual(h.lib.host_manual_move(), 0,
                         "the manual session must be finalized")
        self.assertEqual(h.lib.host_stop_generation(), gen_before,
                         "finalization must not advance the STOP generation")

        self.assertEqual(h.lib.host_mount_stop(), 1)

    # ── Test 6: a rejected single-axis update changes nothing ───────────────

    def test_rejected_single_axis_update_preserves_current_setpoint(self):
        h = self.h
        self.assertEqual(h.lib.host_mount_tracking(TRACKING_SIDEREAL), 1)
        h.wait_status(STATUS_TRACKING)
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.5, 0.3), 1)
        h.wait_status(STATUS_SLEWING)
        self.assertEqual(h.lib.host_saved_tracking(), TRACKING_SIDEREAL)
        session_before = h.lib.host_manual_session()
        version_before = h.lib.host_move_axis_version()
        ra_before = h.lib.host_move_axis_ra_dps()
        dec_before = h.lib.host_move_axis_dec_dps()

        # Fill the command queue with real SLEW orders so a single-axis update
        # hits a genuine queue-full rejection (the executor is busy slewing, so
        # the orders stay queued).
        self.assertEqual(h.lib.host_fill_queue_with_slew(10), 10)

        self.assertEqual(h.lib.host_mount_move_axis_rate(1, 0.7), 0,
                         "the single-axis update must be rejected")

        self.assertEqual(h.lib.host_move_axis_ra_dps(), ra_before,
                         "a rejected update must not change RA")
        self.assertEqual(h.lib.host_move_axis_dec_dps(), dec_before,
                         "a rejected update must not change DEC")
        self.assertEqual(h.lib.host_move_axis_version(), version_before,
                         "a rejected update must not change the version")
        self.assertEqual(h.lib.host_manual_session(), session_before,
                         "a rejected update must not change the session")
        self.assertEqual(h.lib.host_saved_tracking(), TRACKING_SIDEREAL,
                         "a rejected update must not change saved_tracking")

        h.reset()

    # ── swapped Test 1: concurrent DEC then RA preserve both ────────────────

    def test_concurrent_dec_ra_updates_preserve_both_rates(self):
        h = self.h
        h.lib.host_arm_first_producer_gate(1)
        h.lib.host_clear_first_producer_gate()

        results = []

        def update_dec():
            results.append(h.lib.host_mount_move_axis_rate(1, 0.3))

        t = threading.Thread(target=update_dec)
        t.start()
        h.wait_until(lambda: h.lib.host_reached_first_producer_gate() == 1,
                     2.0, "DEC producer reached the before-enqueue hook")

        self.assertEqual(h.lib.host_mount_move_axis_rate(0, 0.5), 1)
        h.lib.host_release_first_producer()

        t.join(timeout=5.0)
        self.assertEqual(results, [1], "the DEC update must be accepted")

        self.assertAlmostEqual(h.lib.host_move_axis_ra_dps(), 0.5, places=6,
                               msg="RA must keep its own rate")
        self.assertAlmostEqual(h.lib.host_move_axis_dec_dps(), 0.3, places=6,
                               msg="DEC must keep its own rate")
        self.assertEqual(h.lib.host_manual_move(), 1)

        self.assertEqual(h.lib.host_mount_move_axis_speed(0.0, 0.0), 1)
        h.wait_status(STATUS_READY)

    # ── swapped Test 2: zeroing DEC does not lose RA's update ────────────────

    def test_zero_dec_does_not_overwrite_concurrent_ra_update(self):
        h = self.h
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.5, 0.3), 1)
        h.wait_status(STATUS_SLEWING)

        h.lib.host_arm_first_producer_gate(1)
        h.lib.host_clear_first_producer_gate()

        results = []

        def zero_dec():
            results.append(h.lib.host_mount_move_axis_rate(1, 0.0))

        t = threading.Thread(target=zero_dec)
        t.start()
        h.wait_until(lambda: h.lib.host_reached_first_producer_gate() == 1,
                     2.0, "DEC zero reached the before-enqueue hook")

        self.assertEqual(h.lib.host_mount_move_axis_rate(0, 0.7), 1)
        h.lib.host_release_first_producer()

        t.join(timeout=5.0)
        self.assertEqual(results, [1], "the DEC zero must be accepted")

        self.assertAlmostEqual(h.lib.host_move_axis_ra_dps(), 0.7, places=6,
                               msg="RA must keep its most recent rate")
        self.assertEqual(h.lib.host_move_axis_dec_dps(), 0.0,
                         "DEC must be stopped")
        self.assertEqual(h.lib.host_manual_move(), 1,
                         "the session must remain valid (RA still moving)")

        self.assertEqual(h.lib.host_mount_move_axis_speed(0.0, 0.0), 1)
        h.wait_status(STATUS_READY)

    # ── single-axis finalization propagates a drain failure (same mechanism) ─

    def test_single_axis_finalization_reports_drain_failure(self):
        h = self.h
        # Absent RA completion from the start: the forward motion's batches never
        # complete, so the single-axis zero → finalize drain times out.
        h.lib.host_rmt_set_absent(0)
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.5, 0.0), 1)
        h.wait_status(STATUS_SLEWING)

        # Zeroing RA with DEC already zero finalizes the session; the failed
        # drain must be reported as an error, not success.
        self.assertEqual(h.lib.host_mount_move_axis_rate(0, 0.0), 0,
                         "a failed drain through the single-axis finalization "
                         "must be reported as an error")
        self.assertEqual(h.lib.host_finished_manual_aborted(), 1,
                         "the failed drain's result must be recorded")

        h.lib.host_rmt_clear_faults()
        h.reset()

    # ── Test 7: successive updates of the same axis follow admission order ──

    def test_same_axis_updates_follow_admission_order(self):
        h = self.h
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.0, 0.3), 1)
        h.wait_status(STATUS_SLEWING)
        session_before = h.lib.host_manual_session()

        for rate in (0.1, 0.2, 0.3, 0.4):
            self.assertEqual(h.lib.host_mount_move_axis_rate(0, rate), 1)

        self.assertAlmostEqual(h.lib.host_move_axis_ra_dps(), 0.4, places=6,
                         msg="RA must keep the last admitted update")
        self.assertAlmostEqual(h.lib.host_move_axis_dec_dps(), 0.3, places=6,
                         msg="DEC must remain unchanged")
        self.assertEqual(h.lib.host_manual_session(), session_before,
                         "no new session may be created per update")

        self.assertEqual(h.lib.host_mount_move_axis_speed(0.0, 0.0), 1)
        h.wait_status(STATUS_READY)

    # ── Test 8: an old finalization does not cancel a new session ───────────

    def test_axis_zero_finalization_cannot_cancel_new_manual_session(self):
        h = self.h
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.5, 0.0), 1)
        h.wait_status(STATUS_SLEWING)
        self.assertEqual(h.lib.host_manual_move(), 1)

        h.lib.host_set_pause_after_move_axis_stop_open(1)
        h.lib.host_clear_move_axis_stop_open_hook()

        results = []

        def finalize():
            results.append(h.lib.host_mount_move_axis_rate(0, 0.0))

        t = threading.Thread(target=finalize)
        t.start()
        h.wait_until(lambda: h.lib.host_reached_move_axis_stop_open_hook() == 1,
                     2.0, "finalization reached the post-drain hook")

        # A fresh session is admitted while the old finalization is suspended.
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.3, 0.0), 1)
        self.assertEqual(h.lib.host_manual_move(), 1,
                         "the new session must be active")

        h.lib.host_set_pause_after_move_axis_stop_open(0)
        t.join(timeout=5.0)
        self.assertEqual(results, [1], "the finalization must report success")

        self.assertEqual(h.lib.host_manual_move(), 1,
                         "the old finalization must not clear the new session")

        self.assertEqual(h.lib.host_mount_move_axis_speed(0.0, 0.0), 1)
        h.wait_status(STATUS_READY)


if __name__ == "__main__":
    unittest.main()
