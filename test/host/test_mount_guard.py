"""Host tests — redundant manual-zero guard and guide-rate validation.

These drive the production mount layer (mount_set_move_axis_speed,
mount_pulse_guide) against the production motors scheduler/queue, with RTOS and
RMT mocked, and assert observable state — not a stub that fakes movement.
"""
import math
import unittest

from harness import (Harness, STATUS_READY, STATUS_TRACKING, STATUS_SLEWING,
                     TRACKING_SIDEREAL, NORTH, EAST, WEST, SIDEREAL_DPS,
                     GUIDE_DPS)


class MountGuardTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.h = Harness()

    def setUp(self):
        self.h.reset()

    # ── Section 1: redundant manual zero must not cancel tracking/guiding ──

    def test_redundant_zero_preserves_tracking_and_guiding(self):
        h = self.h
        self.assertEqual(h.lib.host_mount_tracking(TRACKING_SIDEREAL), 1)
        h.wait_status(STATUS_TRACKING)

        self.assertEqual(h.lib.host_mount_guide(NORTH, 500), 1)
        h.wait_until(lambda: h.lib.host_guiding() == 1, 1.0, "guiding=true")

        # A zero with no manual move in flight must be a no-op.
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.0, 0.0), 1)
        self.assertEqual(h.lib.host_status(), STATUS_TRACKING)
        self.assertEqual(h.lib.host_tracking(), TRACKING_SIDEREAL)
        self.assertEqual(h.lib.host_guiding(), 1)

        # A second consecutive release must not cancel the tracking either.
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.0, 0.0), 1)
        self.assertEqual(h.lib.host_status(), STATUS_TRACKING)
        self.assertEqual(h.lib.host_tracking(), TRACKING_SIDEREAL)

        # The pulse completes on its own and tracking survives it.
        h.wait_not_guiding()
        self.assertEqual(h.lib.host_status(), STATUS_TRACKING)
        self.assertEqual(h.lib.host_tracking(), TRACKING_SIDEREAL)

    def test_manual_stop_restores_tracking_once(self):
        h = self.h
        self.assertEqual(h.lib.host_mount_tracking(TRACKING_SIDEREAL), 1)
        h.wait_status(STATUS_TRACKING)

        # Start a real manual move: this pauses tracking.
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.5, 0.0), 1)
        h.wait_status(STATUS_SLEWING)

        # Release the last active manual axis: tracking is restored.
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.0, 0.0), 1)
        h.wait_status(STATUS_TRACKING)
        self.assertEqual(h.lib.host_tracking(), TRACKING_SIDEREAL)

        # A second release is redundant and must not drop the TRACK again.
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.0, 0.0), 1)
        self.assertEqual(h.lib.host_status(), STATUS_TRACKING)
        self.assertEqual(h.lib.host_tracking(), TRACKING_SIDEREAL)

    def test_explicit_stop_still_stops_everything(self):
        h = self.h
        self.assertEqual(h.lib.host_mount_tracking(TRACKING_SIDEREAL), 1)
        h.wait_status(STATUS_TRACKING)
        self.assertEqual(h.lib.host_mount_stop(), 1)
        self.assertEqual(h.lib.host_status(), STATUS_READY)
        self.assertEqual(h.lib.host_tracking(), 0)   # TRACKING_NONE

    # ── Section 2: guide-rate validation ──

    def test_default_guide_rate_is_half_sidereal(self):
        h = self.h
        self.assertAlmostEqual(h.lib.host_get_guide_rate_ra(),
                               SIDEREAL_DPS * 0.5, places=7)
        self.assertAlmostEqual(h.lib.host_get_guide_rate_dec(),
                               SIDEREAL_DPS * 0.5, places=7)

    def test_invalid_rates_rejected_without_change(self):
        h = self.h
        default = h.lib.host_get_guide_rate_ra()
        self.assertEqual(h.lib.host_set_guide_rate_ra(-0.1), 0)
        self.assertEqual(h.lib.host_set_guide_rate_ra(1.5), 0)
        self.assertEqual(h.lib.host_set_guide_rate_ra(float("nan")), 0)
        self.assertEqual(h.lib.host_set_guide_rate_ra(float("inf")), 0)
        self.assertEqual(h.lib.host_get_guide_rate_ra(), default)

        default_dec = h.lib.host_get_guide_rate_dec()
        self.assertEqual(h.lib.host_set_guide_rate_dec(-0.1), 0)
        self.assertEqual(h.lib.host_set_guide_rate_dec(1.5), 0)
        self.assertEqual(h.lib.host_set_guide_rate_dec(float("nan")), 0)
        self.assertEqual(h.lib.host_get_guide_rate_dec(), default_dec)

    def test_valid_rate_accepted(self):
        h = self.h
        self.assertEqual(h.lib.host_set_guide_rate_ra(0.001), 1)
        self.assertAlmostEqual(h.lib.host_get_guide_rate_ra(), 0.001, places=6)
        self.assertEqual(h.lib.host_set_guide_rate_ra(1.0), 1)   # ceiling
        self.assertAlmostEqual(h.lib.host_get_guide_rate_ra(), 1.0, places=6)

    def test_zero_rate_disables_axis_but_not_the_other(self):
        h = self.h
        # Zero is a valid "guiding disabled" value.
        self.assertEqual(h.lib.host_set_guide_rate_dec(0.0), 1)

        # North (DEC axis) is rejected before enqueueing.
        self.assertEqual(h.lib.host_mount_guide(NORTH, 500), 0)
        self.assertEqual(h.lib.host_status(), STATUS_READY)

        # East (RA axis) still works — its rate is untouched.
        self.assertEqual(h.lib.host_mount_guide(EAST, 200), 1)
        h.wait_until(lambda: h.lib.host_guiding() == 1, 1.0, "east guiding=true")
        h.wait_not_guiding()

        # West also still works.
        self.assertEqual(h.lib.host_mount_guide(WEST, 200), 1)
        h.wait_not_guiding()


if __name__ == "__main__":
    unittest.main()
