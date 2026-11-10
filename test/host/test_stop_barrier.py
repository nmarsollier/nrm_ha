"""Host tests — STOP is a barrier: it must not report READY before the motion
task has actually finished.

The production motion task runs in a real thread while motors_stop() runs in
the test thread.  Without a stop barrier, motors_stop() sets status to READY
and returns before the motion task has left its loop and torn down RMT, so a
caller can observe READY (and enqueue a new move) while the mount is still
finishing.  The barrier makes motors_stop() wait for the motion task's
post-abort cleanup — which marks an interrupted batched move's position as
uncertain — so that flag is already visible the instant stop returns.
"""
import unittest

from harness import Harness, STATUS_SLEWING


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

        # STOP must not return until the motion task has exited its loop and run
        # the post-abort cleanup that marks the interrupted move's position
        # uncertain.  The motion task takes ~1 ms to notice the abort, far longer
        # than the ctypes round-trip that reads the flag next, so a missing
        # barrier deterministically reads 0 here.
        self.assertEqual(h.lib.host_mount_stop(), 1)
        self.assertEqual(h.lib.host_position_uncertain(), 1)


if __name__ == "__main__":
    unittest.main()
