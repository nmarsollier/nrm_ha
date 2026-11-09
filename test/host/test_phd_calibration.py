"""Host — PHD2-style PulseGuide calibration against the production scheduler.

Runs the real mount/motors C (queue + motion loop + RMT encoding) with RTOS/RMT
mocked, drives a canonical guide sequence, and verifies the observed STEP
transmissions and position counters against an independent integral oracle.
Also injects RMT failures to prove they are observable and distinguishable.
"""
import time
import unittest

from harness import (Harness, STATUS_READY, STATUS_TRACKING, STATUS_ERROR,
                     TRACKING_NONE, TRACKING_SIDEREAL,
                     NORTH, SOUTH, EAST, WEST,
                     SIDEREAL_DPS, GUIDE_DPS, DEG_PER_STEP)

# DIR GPIO level recorded for each DEC direction by motors_hw_set_direction_dec:
# POSITIVE → GPIO 9 = 1, NEGATIVE → GPIO 9 = 0.
DEC_POS = 1
DEC_NEG = 0


class PhdCalibrationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.h = Harness()

    def setUp(self):
        self.h.reset()
        self.h.lib.host_set_dec_deg(90.0)   # DEC axis +90°, far from pole/limits

    def start_tracking(self):
        self.assertEqual(self.h.lib.host_mount_tracking(TRACKING_SIDEREAL), 1)
        self.h.wait_status(STATUS_TRACKING)
        time.sleep(0.2)   # let the tracking loop reach steady state

    def pulse(self, direction, duration_ms):
        """Issue one guide pulse, confirm start and end, return axis deltas
        plus the DEC STEP transmissions observed during the pulse window."""
        d0 = self.h.lib.host_dec_steps()
        r0 = self.h.lib.host_ra_steps()
        dec_idx0 = len(self.h.dec_steps_recorded())
        self.assertEqual(self.h.lib.host_mount_guide(direction, duration_ms), 1,
                         f"guide {direction} rejected")
        self.h.wait_until(lambda: self.h.lib.host_guiding() == 1, 1.0, "guiding=true")
        self.h.wait_not_guiding(timeout=duration_ms / 1000.0 + 1.5)
        return (self.h.lib.host_dec_steps() - d0,
                self.h.lib.host_ra_steps() - r0,
                self.h.dec_steps_recorded()[dec_idx0:])

    # ── baseline ──

    def test_tracking_baseline(self):
        self.start_tracking()
        r0 = self.h.lib.host_ra_steps()
        d0 = self.h.lib.host_dec_steps()
        time.sleep(1.0)
        ra_delta = self.h.lib.host_ra_steps() - r0
        dec_delta = self.h.lib.host_dec_steps() - d0
        expected = SIDEREAL_DPS * 1.0 / DEG_PER_STEP   # ~44.6 steps
        self.assertGreater(ra_delta, expected - 6, "RA should advance at sidereal")
        self.assertLess(ra_delta, expected + 6, "RA advanced off-rate")
        self.assertEqual(dec_delta, 0, "DEC must stay still during pure tracking")

    # ── magnitude and sign ──

    def test_north_pulse_magnitude_and_sign(self):
        self.start_tracking()
        dec_delta, ra_delta, dec_rec = self.pulse(NORTH, 500)
        expected = GUIDE_DPS * 0.5 / DEG_PER_STEP    # 11.1415307 steps
        self.assertGreater(dec_delta, 0, "North must increase mechanical DEC")
        self.assertLess(abs(dec_delta - expected), 2.0,
                        f"DEC delta {dec_delta} != ~{expected:.2f}")
        # Observed STEPs must match the counter and travel in +DEC, so a
        # physically silent DEC (RMT ok but no pulse) fails here — not only in
        # the dedicated suppression test.
        self.assertEqual(len(dec_rec), dec_delta,
                         "DEC STEP records != counter delta")
        self.assertGreater(len(dec_rec), 0, "no DEC STEP transmitted")
        self.assertTrue(all(s.dir_gpio == DEC_POS for s in dec_rec),
                        "North STEP direction must be +DEC")
        # A DEC pulse must not add an RA correction: RA delta is base tracking.
        self.assertGreater(ra_delta, 0)

    def test_south_pulse_opposes_north(self):
        self.start_tracking()
        dec_delta, _, dec_rec = self.pulse(SOUTH, 500)
        expected = GUIDE_DPS * 0.5 / DEG_PER_STEP
        self.assertLess(dec_delta, 0, "South must decrease mechanical DEC")
        self.assertLess(abs(abs(dec_delta) - expected), 2.0)
        self.assertEqual(len(dec_rec), abs(dec_delta),
                         "DEC STEP records != |counter delta|")
        self.assertTrue(all(s.dir_gpio == DEC_NEG for s in dec_rec),
                        "South STEP direction must be -DEC")

    def test_east_decelerates_ra_without_reversing(self):
        # With sidereal tracking, East subtracts from the RA base rate but never
        # reverses it: RA keeps advancing at 0.5x sidereal during the pulse.
        self.start_tracking()
        ra0 = self.h.lib.host_ra_steps()
        self.assertEqual(self.h.lib.host_mount_guide(EAST, 500), 1)
        self.h.wait_until(lambda: self.h.lib.host_guiding() == 1, 1.0, "guiding=true")
        self.h.wait_not_guiding(timeout=1.5)
        ra_delta = self.h.lib.host_ra_steps() - ra0
        self.assertGreater(ra_delta, 0, "East must not reverse mechanical RA")
        # ~0.5x sidereal over ~0.5 s ≈ 11 steps, well below full sidereal (~22).
        self.assertLess(ra_delta, 20, "East did not decelerate RA")

    # ── canonical DEC round-trip ──

    def test_dec_12_pulse_round_trip(self):
        self.start_tracking()
        origin = self.h.lib.host_dec_steps()

        for _ in range(12):
            dec_delta, _, dec_rec = self.pulse(NORTH, 500)
            self.assertGreater(dec_delta, 0)
            self.assertEqual(len(dec_rec), dec_delta)
        north_total = self.h.lib.host_dec_steps() - origin
        expected_12 = 12 * GUIDE_DPS * 0.5 / DEG_PER_STEP
        self.assertLess(abs(north_total - expected_12), 3.0,
                        f"12 North total {north_total} != ~{expected_12:.1f}")

        for _ in range(12):
            dec_delta, _, dec_rec = self.pulse(SOUTH, 500)
            self.assertLess(dec_delta, 0)
            self.assertEqual(len(dec_rec), -dec_delta)
        final = self.h.lib.host_dec_steps() - origin
        self.assertLess(abs(final), 3.0, f"12S did not return to origin ({final})")

    # ── RMT observability and fault injection ──

    def test_suppress_dec_makes_nominal_fail(self):
        # A physically silent DEC (RMT returns OK but no STEP reaches the motor)
        # must be visible in the observed transmissions even though the firmware
        # counter still advances.  This is the failure the nominal case detects.
        self.start_tracking()
        self.h.lib.host_rmt_set_suppress(1)   # suppress DEC
        dec_delta, _, dec_rec = self.pulse(NORTH, 500)
        self.assertGreater(dec_delta, 0, "firmware counter advanced")
        self.assertEqual(len(dec_rec), 0,
                         "DEC STEP transmissions were suppressed — nominal FAILS")
        # RA still transmits (only DEC was suppressed).
        self.assertGreater(len(self.h.ra_steps_recorded()), 0)

    def test_rmt_tx_error_enters_error_state(self):
        self.h.lib.host_rmt_set_tx_error(1)   # DEC
        self.assertEqual(self.h.lib.host_mount_guide(NORTH, 500), 1)
        self.h.wait_status(STATUS_ERROR, timeout=3.0)
        self.assertEqual(self.h.lib.host_has_hardware_fault(), 1,
                         "RMT tx error must latch a hardware fault")

    def test_absent_completion_enters_error_state(self):
        self.h.lib.host_rmt_set_absent(1)     # DEC: TX ok, completion never fires
        self.assertEqual(self.h.lib.host_mount_guide(NORTH, 500), 1)
        # The wait times out (~2 s) and the motion task enters ERROR.
        self.h.wait_status(STATUS_ERROR, timeout=5.0)
        self.assertEqual(self.h.lib.host_has_hardware_fault(), 1,
                         "RMT completion timeout must latch a hardware fault")

    def test_hardware_fault_survives_power_recovery(self):
        # A genuine hardware fault is unrecoverable: the power-loss recovery
        # path must NOT return the mount to READY, or it could resume with an
        # uncertain position after a hardware failure.
        self.h.lib.host_rmt_set_tx_error(1)   # DEC
        self.assertEqual(self.h.lib.host_mount_guide(NORTH, 500), 1)
        self.h.wait_status(STATUS_ERROR, timeout=3.0)
        self.assertEqual(self.h.lib.host_has_hardware_fault(), 1)
        self.h.lib.host_update_power(0)       # power absent
        self.assertEqual(self.h.lib.host_status(), STATUS_ERROR)
        self.h.lib.host_update_power(1)       # power returns
        self.assertEqual(self.h.lib.host_status(), STATUS_ERROR,
                         "hardware fault must survive power recovery")

    def test_error_clears_tracking_mode(self):
        # A hardware fault while tracking must also clear the tracking mode, so
        # the snapshot is coherent (ERROR, never ERROR + sidereal).
        self.assertEqual(self.h.lib.host_mount_tracking(TRACKING_SIDEREAL), 1)
        self.h.wait_status(STATUS_TRACKING)
        self.h.lib.host_rmt_set_tx_error(0)   # RA: next tracking step fails
        self.h.wait_status(STATUS_ERROR, timeout=3.0)
        self.assertEqual(self.h.lib.host_tracking(), TRACKING_NONE,
                         "ERROR must clear the tracking mode")
        self.assertEqual(self.h.lib.host_has_hardware_fault(), 1)

    def test_slew_rmt_failure_is_hardware_fault(self):
        # An RMT transmit failure during a slew (MoveAxis, batched) is the same
        # hardware fault as during tracking: ERROR + latched, never a clean READY.
        self.h.lib.host_rmt_set_tx_error(0)   # RA
        self.assertEqual(self.h.lib.host_mount_move_axis_speed(0.5, 0.0), 1)
        self.h.wait_status(STATUS_ERROR, timeout=3.0)
        self.assertEqual(self.h.lib.host_has_hardware_fault(), 1)

    def test_power_fault_without_hardware_fault_is_recoverable(self):
        # A pure power loss is recoverable: with no latched hardware fault,
        # power returning clears ERROR back to READY.
        self.h.lib.host_update_power(0)
        self.assertEqual(self.h.lib.host_status(), STATUS_ERROR)
        self.h.lib.host_update_power(1)
        self.assertEqual(self.h.lib.host_status(), STATUS_READY)

    def test_dec_limit_truncates_guide_and_flags_it(self):
        # DEC at its upper mechanical limit: the guide is accepted but the very
        # first step would exceed the limit, so it is truncated to zero movement
        # and must flag it — distinguishable from a normal completion.
        self.h.lib.host_set_dec_deg(150.0)    # DEC max
        d0 = self.h.lib.host_dec_steps()
        self.assertEqual(self.h.lib.host_mount_guide(NORTH, 500), 1)
        self.h.wait_until(lambda: self.h.lib.host_guiding() == 1, 1.0, "guiding=true")
        self.h.wait_not_guiding(timeout=1.5)
        self.assertEqual(self.h.lib.host_guide_truncated(), 1,
                         "a limit-truncated guide must set guide_truncated")
        self.assertEqual(self.h.lib.host_dec_steps(), d0,
                         "truncated guide must not move DEC")

    def test_normal_guide_does_not_flag_truncation(self):
        self.h.lib.host_set_dec_deg(90.0)     # clear of limits
        self.assertEqual(self.h.lib.host_mount_guide(NORTH, 500), 1)
        self.h.wait_until(lambda: self.h.lib.host_guiding() == 1, 1.0, "guiding=true")
        self.h.wait_not_guiding(timeout=1.5)
        self.assertEqual(self.h.lib.host_guide_truncated(), 0,
                         "a completed guide must not flag truncation")

    def test_ra_limit_clears_guide_and_guiding(self):
        # A West guide at the RA limit hits the limit on the first step: the
        # guide is truncated, guiding must clear (not stay stuck true), and the
        # truncation must be flagged.
        self.h.lib.host_set_ra_deg(100.0)   # RA max
        self.assertEqual(self.h.lib.host_mount_guide(WEST, 500), 1)
        self.h.wait_until(lambda: self.h.lib.host_guiding() == 1, 1.0, "guiding=true")
        self.h.wait_not_guiding(timeout=1.5)
        self.assertEqual(self.h.lib.host_guiding(), 0,
                         "guiding must clear when a guide hits the RA limit")
        self.assertEqual(self.h.lib.host_guide_truncated(), 1,
                         "a limit-truncated RA guide must flag truncation")

    def test_ra_limit_during_tracking_truncates_dec_guide(self):
        # Tracking advances RA into the limit while a North DEC guide is active.
        # The RA limit must also end the DEC guide: guiding clears and the
        # truncation is flagged (the DEC guide was interrupted, not completed).
        self.h.lib.host_set_ra_deg(99.999)   # ~11 steps of headroom (~250 ms)
        self.assertEqual(self.h.lib.host_mount_tracking(TRACKING_SIDEREAL), 1)
        self.h.wait_status(STATUS_TRACKING)
        self.assertEqual(self.h.lib.host_mount_guide(NORTH, 1000), 1)
        self.h.wait_until(lambda: self.h.lib.host_guiding() == 1, 1.0, "guiding=true")
        self.h.wait_status(STATUS_READY, timeout=3.0)
        self.assertEqual(self.h.lib.host_guiding(), 0,
                         "guiding must clear when tracking hits the RA limit")
        self.assertEqual(self.h.lib.host_guide_truncated(), 1,
                         "an interrupted DEC guide must flag truncation")

    def test_stop_cancels_active_pulse(self):
        self.start_tracking()
        self.assertEqual(self.h.lib.host_mount_guide(NORTH, 2000), 1)
        self.h.wait_until(lambda: self.h.lib.host_guiding() == 1, 1.0, "guiding=true")
        self.assertEqual(self.h.lib.host_mount_stop(), 1)
        self.h.wait_status(STATUS_READY)
        self.assertEqual(self.h.lib.host_guiding(), 0)
        # The pulse was cut short, far below a full 2000 ms (~44 steps).
        self.assertLess(len(self.h.dec_steps_recorded()), 44)

    # ── notification wake-up latency ──

    def test_pulse_wakes_tracking_loop_promptly(self):
        self.start_tracking()
        time.sleep(0.3)
        max_latency = 0.0
        for _ in range(8):
            t0 = time.monotonic()
            self.assertEqual(self.h.lib.host_mount_guide(NORTH, 100), 1)
            latency = None
            while time.monotonic() - t0 < 1.0:
                if self.h.lib.host_guiding() == 1:
                    latency = time.monotonic() - t0
                    break
                time.sleep(0.001)
            self.assertIsNotNone(latency, "guide never started")
            max_latency = max(max_latency, latency)
            self.h.wait_not_guiding(timeout=1.0)
        # xTaskNotifyGive wakes the loop immediately; a lost notification would
        # wait out the ~20 ms tracking sleep before the queue peek sees it.
        self.assertLess(max_latency, 0.015,
                        f"guide wake latency {max_latency*1e3:.1f} ms too high")


if __name__ == "__main__":
    unittest.main()
