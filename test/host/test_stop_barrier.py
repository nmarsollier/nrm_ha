"""Host tests — STOP is a barrier: it must not report READY before the motion
task has actually finished.

The production motion task runs in a real thread while motors_stop() runs in
the test thread.  Without a stop barrier, motors_stop() could return before the
motion task has left its loop and drained RMT, so a caller could enqueue a new
move while the mount is still finishing.  PR5 makes STOP graceful: it drains the
in-flight batches (exact position, no uncertainty) and publishes READY only
after the motion task is idle.
"""
import unittest

from harness import Harness, STATUS_READY, STATUS_SLEWING


class StopBarrierTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.h = Harness()

    def setUp(self):
        self.h.reset()

    def test_stop_waits_for_motion_task_to_finish(self):
        h = self.h
        # A continuous manual move (batched RMT) keeps the motion task in its
        # slewing loop until a stop arrives.
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.5, 0.0), 1)
        h.wait_status(STATUS_SLEWING)

        # STOP must not return until the motion task has exited its loop and
        # drained the in-flight batches.  The graceful drain preserves the exact
        # position, so no uncertainty is flagged.
        self.assertEqual(h.lib.host_mount_stop(), 1)
        self.assertEqual(h.lib.host_status(), STATUS_READY)
        self.assertEqual(h.lib.host_position_uncertain(), 0)
        # The graceful drain accounts every emitted pulse exactly: the counter
        # equals the number of STEP transmissions observed (fine resolution, so
        # one pulse == one canonical microstep).
        self.assertEqual(h.lib.host_ra_steps(), len(h.ra_steps_recorded()),
                         "RA counter != emitted RA pulses")


if __name__ == "__main__":
    unittest.main()
