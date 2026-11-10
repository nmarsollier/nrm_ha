"""Host tests — a saturated command queue is reported as a rejection.

motors_queue_put returns false when the queue (depth 10) is full, and every
producer maps that to a non-OK result, so an operation never reports success
when its command was dropped.  This holds the motion task in a continuous
MoveAxis loop (which does not dequeue TRACK commands) and enqueues past the
queue depth, then asserts the overflow is surfaced as a rejection.
"""
import unittest

from harness import Harness, STATUS_SLEWING, TRACKING_SIDEREAL


class QueueSaturationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.h = Harness()

    def setUp(self):
        self.h.reset()

    def test_queue_full_rejects_command(self):
        h = self.h
        # A continuous MoveAxis keeps the motion task in its slew loop, which
        # does not dequeue the TRACK commands we are about to enqueue.
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.5, 0.0), 1)
        h.wait_status(STATUS_SLEWING)

        results = [h.lib.host_mount_tracking(TRACKING_SIDEREAL) for _ in range(12)]

        self.assertEqual(h.lib.host_mount_move_axis_speed(0.0, 0.0), 1)
        h.wait_idle_status()

        self.assertIn(0, results,
                      "a full queue must reject new commands, not report success")


if __name__ == "__main__":
    unittest.main()
