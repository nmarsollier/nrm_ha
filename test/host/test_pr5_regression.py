"""PR5 deterministic regression tests.

Each test uses a controlled synchronization point (weak motors_test_hook_*
no-ops overridden by the harness) to reproduce a race deterministically — not by
sleeping.  A test that reproduces a confirmed defect FAILS against the current
implementation; a test that passes is evidence that the race is handled
correctly.
"""
import threading
import time
import unittest

from harness import (Harness, STATUS_READY, STATUS_TRACKING, STATUS_SLEWING,
                     TRACKING_SIDEREAL, TRACKING_NONE)


class Pr5RegressionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.h = Harness()

    def setUp(self):
        self.h.reset()

    # ── Defect 1 (obsolete tracking restore after STOP) — deterministic hook ──

    def test_stop_between_enqueue_and_manual_session_publication(self):
        h = self.h
        self.assertEqual(h.lib.host_mount_tracking(TRACKING_SIDEREAL), 1)
        h.wait_status(STATUS_TRACKING)

        results = []
        h.lib.host_set_pause_after_enqueue(1)
        h.lib.host_clear_enqueue_hook()

        def producer():
            results.append(h.lib.host_mount_move_axis_speed(0.5, 0.0))

        t = threading.Thread(target=producer)
        t.start()

        h.wait_until(lambda: h.lib.host_reached_enqueue_hook() == 1,
                     2.0, "producer reached the enqueue hook")

        self.assertEqual(h.lib.host_mount_stop(), 1)

        h.lib.host_set_pause_after_enqueue(0)
        t.join()
        self.assertEqual(results, [1], "the MoveAxis producer must report success")

        # A redundant zero must NOT restore the tracking the STOP just cancelled.
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.0, 0.0), 1)
        self.assertEqual(h.lib.host_tracking(), TRACKING_NONE,
                         "STOP must cancel the tracking; a later zero must not "
                         "restore it")

    # ── STOP after dequeue and before activation: the command must not run. ──

    def test_stop_between_dequeue_and_activation(self):
        h = self.h
        h.lib.host_set_pause_after_process_command(1)
        h.lib.host_clear_process_command_hook()
        h.lib.host_set_pause_after_stop_begin(1)
        h.lib.host_clear_stop_begin_hook()

        self.assertEqual(h.lib.host_mount_move_axis_speed(0.5, 0.0), 1)

        # The motion task dequeued and validated the command, then paused before
        # activating its motion loop.
        h.wait_until(lambda: h.lib.host_reached_process_command_hook() == 1,
                     2.0, "motion task reached the process-command hook")

        stop_results = []

        def stopper():
            stop_results.append(h.lib.host_mount_stop())

        t_stop = threading.Thread(target=stopper)
        t_stop.start()

        # The STOP closed admission and set active=false, then paused.
        h.wait_until(lambda: h.lib.host_reached_stop_begin_hook() == 1,
                     2.0, "STOP reached the stop-begin hook")

        # Release the motion task so it checks active (false) and never runs.
        h.lib.host_set_pause_after_process_command(0)
        # Release the STOP so it completes.
        h.lib.host_set_pause_after_stop_begin(0)
        t_stop.join()

        self.assertEqual(stop_results, [1], "STOP must succeed")
        h.wait_status(STATUS_READY)
        self.assertEqual(h.lib.host_step_count(), 0,
                         "a command invalidated by STOP must not emit STEP pulses")

    # ── Defect 9 (RMT accounting) — counters == emitted pulses, signed ────────

    def test_counters_match_emitted_pulses_both_axes(self):
        h = self.h
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.5, -0.3), 1)
        h.wait_status(STATUS_SLEWING)
        time.sleep(0.2)

        self.assertEqual(h.lib.host_mount_stop(), 1)
        self.assertEqual(h.lib.host_position_uncertain(), 0)
        self.assertEqual(h.lib.host_ra_steps(), len(h.ra_steps_recorded()))
        self.assertEqual(h.lib.host_dec_steps(), -len(h.dec_steps_recorded()))


if __name__ == "__main__":
    unittest.main()
