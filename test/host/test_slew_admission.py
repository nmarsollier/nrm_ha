"""Host tests — PR5/E12: a SLEW must not disturb tracking or queued work.

The producers (`motors_slew_to_angle` / `motors_slew_axis`) used to call the
heavy `motors_stop()` to pause tracking *before* attempting to admit the SLEW.
That synchronous full stop bumps the STOP generation, clears the command queue
and publishes READY — side effects that belong to a STOP, not to the admission
of a new motion.

These tests drive the real production SLEW admission path and verify:
  - a rejected SLEW (invalid destination or full queue) leaves tracking intact;
  - an accepted SLEW transitions out of and back into tracking;
  - an accepted SLEW does NOT bump the STOP generation (the executor, not the
    producer, owns the tracking→SLEW transition).
"""
import unittest

from harness import (Harness, STATUS_TRACKING, STATUS_SLEWING,
                     TRACKING_SIDEREAL, TRACKING_NONE)


class SlewAdmissionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.h = Harness()

    def setUp(self):
        self.h.reset()
        self.h.lib.host_set_dec_deg(90.0)   # DEC +90°, clear of limits

    def _start_tracking(self):
        h = self.h
        self.assertEqual(h.lib.host_mount_tracking(TRACKING_SIDEREAL), 1)
        h.wait_status(STATUS_TRACKING)
        self.assertEqual(h.lib.host_tracking(), TRACKING_SIDEREAL)

    def test_rejected_slew_preserves_tracking(self):
        # An out-of-range target is rejected by validation before any motion is
        # touched: tracking must remain active.
        self._start_tracking()
        self.assertEqual(self.h.lib.host_slew_to_angle(200.0, 20.0, 4), 0,
                         "an out-of-range target must be rejected")
        self.assertEqual(self.h.lib.host_tracking(), TRACKING_SIDEREAL,
                         "a rejected SLEW must not interrupt tracking")
        self.assertEqual(self.h.lib.host_status(), STATUS_TRACKING)

    def test_queue_full_rejection_preserves_tracking(self):
        # A full command queue rejects the SLEW at admission; the rejection must
        # not disturb active tracking.
        self._start_tracking()
        self.assertEqual(self.h.lib.host_fill_queue_with_slew(10), 10)
        self.assertEqual(self.h.lib.host_slew_to_angle(20.0, 20.0, 4), 0,
                         "a full queue must reject the SLEW")
        self.assertEqual(self.h.lib.host_tracking(), TRACKING_SIDEREAL,
                         "a rejected SLEW must not interrupt tracking")
        self.assertEqual(self.h.lib.host_status(), STATUS_TRACKING)

    def test_accepted_slew_transitions_from_tracking(self):
        # An accepted SLEW transitions out of tracking, then resumes it inline
        # on completion (resume_tracking), never leaving the mount stopped.
        self._start_tracking()
        self.assertEqual(self.h.lib.host_slew_to_angle(10.0, 90.0, 4), 1)
        self.h.wait_status(STATUS_SLEWING)
        self.assertEqual(self.h.lib.host_tracking(), TRACKING_NONE,
                         "the slew must pause tracking while running")
        self.h.wait_status(STATUS_TRACKING)
        self.assertEqual(self.h.lib.host_tracking(), TRACKING_SIDEREAL,
                         "the slew must resume tracking on completion")

    def test_slew_does_not_bump_stop_generation(self):
        # Pausing tracking for a slew must be a light transition, not a full
        # STOP: the STOP generation (which invalidates queued work) must not
        # advance just because a valid slew was admitted.
        self._start_tracking()
        gen_before = self.h.lib.host_stop_generation()
        self.assertEqual(self.h.lib.host_slew_to_angle(10.0, 90.0, 4), 1)
        self.assertEqual(self.h.lib.host_stop_generation(), gen_before,
                         "a slew must not bump the STOP generation")


if __name__ == "__main__":
    unittest.main()
