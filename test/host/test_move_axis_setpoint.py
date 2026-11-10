"""Host tests — MoveAxis is a per-axis setpoint, not a queue of orders.

motors_set_move_axis_speed publishes the rate to a shared setpoint and bumps a
version; the motion task reads the setpoint and watches the version, so the
newest rate always wins and obsolete MoveAxis orders never accumulate — even
when a non-MOVE_AXIS command sits in the queue ahead of a newer rate.
"""
import unittest

from harness import Harness, STATUS_SLEWING


class MoveAxisSetpointTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.h = Harness()

    def setUp(self):
        self.h.reset()

    def test_newest_rate_wins(self):
        h = self.h
        # Start RA forward; the motion task emits steps with dir_gpio 0.
        self.assertEqual(h.lib.host_mount_move_axis_speed(1.0, 0.0), 1)
        h.wait_status(STATUS_SLEWING)

        # Reverse the rate.  The motion task must pick up the newest setpoint
        # and emit steps in the new direction (RA negative -> dir_gpio 1).
        self.assertEqual(h.lib.host_mount_move_axis_speed(-1.0, 0.0), 1)
        h.wait_until(
            lambda: any(s.axis == 0 and s.dir_gpio == 1 for s in h.step_records()),
            2.0, "RA reversed direction")

        self.assertEqual(h.lib.host_mount_move_axis_speed(0.0, 0.0), 1)
        h.wait_idle_status()


if __name__ == "__main__":
    unittest.main()
