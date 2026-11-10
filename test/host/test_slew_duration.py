"""Host tests — the slew-duration estimator mirrors the firmware ramp profile.

motors_slew_duration_seconds() is what mount_slew_to_coordinates.c uses to
predict a goto's arrival time and compensate for the sky rotation during the
move.  It must reproduce the ramp profile the motion task actually applies
(constant MIN_SLEW below the short-slew threshold, a speed cap below the
gentle-slew threshold, then a trapezoidal/triangular accel-decel) rather than
assume a constant commanded speed.  Validated against the independent Python
oracle (test/proto/oracle.py) — the same model the serial goto tests trust.
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "proto"))

from oracle import goto_time, slew_time  # noqa: E402

from harness import Harness  # noqa: E402


class SlewDurationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.h = Harness()

    def test_matches_oracle_single_axis(self):
        h = self.h
        for speed in (1.0, 3.0, 4.5, 6.0):
            for dist in (0.0, 0.5, 1.0, 1.9, 2.0, 3.0, 5.0, 7.9, 8.0,
                         10.0, 20.0, 50.0, 100.0):
                want = slew_time(dist, speed)
                got = h.lib.host_slew_duration(dist, speed)
                self.assertAlmostEqual(
                    got, want, delta=1e-3,
                    msg=f"dist={dist} speed={speed}: got {got}, want {want}")

    def test_matches_oracle_two_axis(self):
        h = self.h
        # The goto duration is max(ra, dec); spot-check a few pairs.
        for ra, dec, speed in ((60.0, 3.0, 6.0), (1.0, 40.0, 4.5),
                               (8.0, 8.0, 3.0), (0.5, 0.5, 6.0)):
            want = goto_time(ra, dec, speed)
            got = max(h.lib.host_slew_duration(ra, speed),
                      h.lib.host_slew_duration(dec, speed))
            self.assertAlmostEqual(
                got, want, delta=1e-3,
                msg=f"ra={ra} dec={dec} speed={speed}: got {got}, want {want}")


if __name__ == "__main__":
    unittest.main()
