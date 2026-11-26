"""Host tests — PR5/E08: mutable TMC2209 microstep resolution and canonical
pulse accounting.

The STEP counters always stay in canonical (tracking, 64 µstep) units.  A coarse
slew switches the drivers to the profile's slew microsteps (8), so each emitted
STEP pulse advances the canonical counter by ``canonical / slew = 8`` steps; every
other motion runs at fine resolution (factor 1).  The accounting must use the
factor the batch was actually emitted at — captured once at motion start — never
re-read from the mutable TMC state, so a later resolution change cannot rescale
or reinterpret an earlier batch.

These tests observe the *physical* pulses recorded by the RMT mock and compare
them to the canonical counters using an independent factor (hardcoded 8 / 1 from
the profile's documented 64/8 split), not ``motors_steps_per_pulse()``.
"""
import unittest

from harness import (Harness, STATUS_READY, STATUS_SLEWING, STATUS_TRACKING,
                     STATUS_ERROR, TRACKING_SIDEREAL, TRACKING_NONE)

# Canonical 64 µsteps / slew 8 µsteps → one coarse STEP pulse = 8 canonical steps.
COARSE_FACTOR = 8
FINE_FACTOR = 1


class MicrostepAccountingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.h = Harness()

    def setUp(self):
        self.h.reset()

    def _wait_coarse(self):
        """Wait until the drivers report the coarse (slew) resolution."""
        self.h.wait_until(lambda: self.h.lib.host_microsteps_active() == 8,
                          3.0, "coarse resolution (8 µsteps)")

    # ── Test 1: fine resolution uses factor 1 ──────────────────────────────

    def test_fine_microstep_batches_use_canonical_factor_one(self):
        h = self.h
        # MoveAxis runs at fine (canonical) resolution.
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.5, -0.3), 1)
        h.wait_status(STATUS_SLEWING)
        h.wait_until(lambda: h.lib.host_step_count() > 0, 3.0, "first pulse")
        self.assertEqual(h.lib.host_microsteps_active(), 64)

        self.assertEqual(h.lib.host_mount_stop(), 1)
        h.wait_status(STATUS_READY)

        # Each fine pulse is one canonical step, with the emitted direction.
        self.assertEqual(h.lib.host_ra_steps(), len(h.ra_steps_recorded()),
                         "RA: one canonical step per fine pulse (positive)")
        self.assertEqual(h.lib.host_dec_steps(), -len(h.dec_steps_recorded()),
                         "DEC: one canonical step per fine pulse (negative)")

    # ── Test 2: coarse resolution uses factor 8 ────────────────────────────

    def test_coarse_slew_batches_use_original_canonical_factor(self):
        h = self.h
        self.assertEqual(h.lib.host_enqueue_slew(20.0, 0.0, 4.0), 1)
        self._wait_coarse()
        h.wait_until(lambda: h.lib.host_step_count() > 0, 3.0, "first pulse")

        self.assertEqual(h.lib.host_mount_stop(), 1)
        h.wait_status(STATUS_READY)

        n_ra = len(h.ra_steps_recorded())
        self.assertGreater(n_ra, 0, "coarse slew must emit RA pulses")
        self.assertEqual(h.lib.host_ra_steps(), COARSE_FACTOR * n_ra,
                         "each coarse pulse is 8 canonical steps")
        self.assertEqual(h.lib.host_dec_steps(), 0,
                         "a pure-RA slew must not move DEC")

    def test_coarse_slew_negative_direction(self):
        h = self.h
        self.assertEqual(h.lib.host_enqueue_slew(-20.0, 0.0, 4.0), 1)
        self._wait_coarse()
        h.wait_until(lambda: h.lib.host_step_count() > 0, 3.0, "first pulse")

        self.assertEqual(h.lib.host_mount_stop(), 1)
        h.wait_status(STATUS_READY)

        n_ra = len(h.ra_steps_recorded())
        self.assertGreater(n_ra, 0)
        self.assertEqual(h.lib.host_ra_steps(), -COARSE_FACTOR * n_ra,
                         "negative slew: -8 canonical steps per pulse")

    # ── Test 5: coarse slew → fine tracking preserves canonical position ──

    def test_slew_to_tracking_preserves_canonical_position(self):
        h = self.h
        self.assertEqual(h.lib.host_enqueue_slew(20.0, 0.0, 4.0), 1)
        self._wait_coarse()
        h.wait_until(lambda: h.lib.host_step_count() > 0, 3.0, "first pulse")
        self.assertEqual(h.lib.host_mount_stop(), 1)
        h.wait_status(STATUS_READY)

        n_slew = len(h.ra_steps_recorded())
        steps_after_slew = h.lib.host_ra_steps()
        self.assertEqual(steps_after_slew, COARSE_FACTOR * n_slew)

        # Isolate the tracking pulses from the slew pulses.
        h.lib.host_step_clear()

        # Switch to tracking: the resolution becomes fine and the counter must
        # NOT be rescaled by the switch.
        self.assertEqual(h.lib.host_mount_tracking(TRACKING_SIDEREAL), 1)
        h.wait_status(STATUS_TRACKING)
        self.assertEqual(h.lib.host_microsteps_active(), 64,
                         "tracking must restore the fine resolution")
        self.assertEqual(h.lib.host_ra_steps(), steps_after_slew,
                         "the resolution switch must not rescale the counter")

        # Wait for an observable number of fine tracking pulses, then stop.
        h.wait_until(lambda: h.lib.host_step_count() >= 5, 5.0,
                     "tracking emitted fine pulses")
        self.assertEqual(h.lib.host_mount_stop(), 1)
        h.wait_status(STATUS_READY)

        n_tracking = len(h.ra_steps_recorded())
        self.assertGreater(n_tracking, 0, "tracking must emit fine pulses")
        self.assertEqual(h.lib.host_ra_steps() - steps_after_slew,
                         FINE_FACTOR * n_tracking,
                         "tracking pulses use factor 1, accumulating on the slew")
        # Total displacement is the independent sum of both phases.
        self.assertEqual(h.lib.host_ra_steps(),
                         COARSE_FACTOR * n_slew + FINE_FACTOR * n_tracking,
                         "total displacement is the independent sum of both motions")

    # ── Test 6: STOP during coarse slew preserves the batch factor ─────────

    def test_stop_during_coarse_slew_preserves_batch_factor(self):
        h = self.h
        self.assertEqual(h.lib.host_enqueue_slew(20.0, 20.0, 4.0), 1)
        self._wait_coarse()
        h.wait_until(lambda: h.lib.host_step_count() > 0, 3.0, "first pulse")

        self.assertEqual(h.lib.host_mount_stop(), 1)
        self.assertEqual(h.lib.host_status(), STATUS_READY)
        self.assertEqual(h.lib.host_position_uncertain(), 0,
                         "a clean coarse drain must not flag uncertainty")

        n_ra = len(h.ra_steps_recorded())
        n_dec = len(h.dec_steps_recorded())
        self.assertGreater(n_ra, 0)
        self.assertGreater(n_dec, 0)
        self.assertEqual(h.lib.host_ra_steps(), COARSE_FACTOR * n_ra,
                         "RA coarse batches keep factor 8 after STOP")
        self.assertEqual(h.lib.host_dec_steps(), COARSE_FACTOR * n_dec,
                         "DEC coarse batches keep factor 8 after STOP")

    # ── Test 11: unverified (zero) resolution refuses motion ───────────────

    def test_zero_resolution_refuses_motion(self):
        h = self.h
        # Force the cached resolution to "unverified" (the boot state before
        # tmc2209_hw_init verifies one), then admit motion.
        h.lib.host_tmc_set_microsteps(0)

        self.assertEqual(h.lib.host_mount_move_axis_speed(0.5, 0.0), 1)
        h.wait_status(STATUS_ERROR)

        self.assertEqual(h.lib.host_step_count(), 0,
                         "no pulse may be emitted under an unverified resolution")
        self.assertEqual(h.lib.host_admission_closed(), 1,
                         "a resolution failure must keep admission closed")

    # ── Test 9: failed microstep switch blocks motion ──────────────────────

    def test_failed_microstep_switch_blocks_motion(self):
        h = self.h
        # A coarse slew requires a resolution switch; make that switch fail.
        h.lib.host_tmc_set_microsteps_fail(1)
        self.assertEqual(h.lib.host_enqueue_slew(20.0, 0.0, 4.0), 1)

        h.wait_status(STATUS_ERROR)
        self.assertEqual(h.lib.host_step_count(), 0,
                         "no pulse may be emitted when the switch failed")
        self.assertEqual(h.lib.host_admission_closed(), 1)

    # ── Test 13: repeated coarse/fine transitions don't accumulate error ───

    def test_repeated_coarse_fine_transitions_do_not_accumulate_position_error(self):
        h = self.h
        total_expected = 0

        # Alternate coarse slews and fine MoveAxis in alternating directions, so
        # each phase's factor (8 / 1), direction, and resolution are exercised.
        phases = [
            ("coarse", +20.0, 4.0),
            ("fine",   +0.5, 0.0),
            ("coarse", -20.0, 4.0),
            ("fine",   -0.5, 0.0),
        ]
        for kind, ra, speed in phases:
            if kind == "coarse":
                self.assertEqual(h.lib.host_enqueue_slew(ra, 0.0, speed), 1)
                self._wait_coarse()
            else:
                self.assertEqual(h.lib.host_mount_move_axis_speed(ra, 0.0), 1)
                h.wait_until(lambda: h.lib.host_microsteps_active() == 64, 3.0,
                             "fine resolution (64 µsteps)")

            h.wait_until(lambda: h.lib.host_step_count() > 0, 3.0, "first pulse")
            self.assertEqual(h.lib.host_mount_stop(), 1)
            h.wait_status(STATUS_READY)

            n = len(h.ra_steps_recorded())
            factor = COARSE_FACTOR if kind == "coarse" else FINE_FACTOR
            sign = 1 if ra >= 0 else -1
            total_expected += sign * factor * n
            h.lib.host_step_clear()

        # The canonical counter equals the independent sum of every batch's
        # canonical displacement, computed with each batch's own factor.
        self.assertEqual(h.lib.host_ra_steps(), total_expected,
                         "repeated coarse/fine transitions must not accumulate error")


if __name__ == "__main__":
    unittest.main()
