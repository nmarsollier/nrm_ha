"""Shared slew-ramp module tests (slew_ramp.c).

Verifies the pure ramp velocity/duration against thresholds, the minimum-speed
singular case, zero windows, non-finite inputs, and the concordance between the
analytic duration and a numerical integration of the velocity profile.
"""
import ctypes as C
import math
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]

NRM = (0.8, 2.0, 8.0, 3.2, 5.0, 5.0)     # min, short, gentle, gentle_max, accel, decel
BFT = (0.8, 2.0, 8.0, 3.2, 10.0, 10.0)


class SlewRampTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        lib = Path(cls.tmp.name) / "slew_ramp.so"
        subprocess.run([
            "cc", "-shared", "-fPIC", "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-I" + str(ROOT / "main"), "-I" + str(ROOT / "main/profiles"),
            str(ROOT / "main/motion/slew_ramp.c"),
            str(ROOT / "test/host/slew_ramp_driver.c"),
            "-lm", "-o", str(lib),
        ], check=True)
        cls.lib = C.CDLL(str(lib))
        cls.lib.host_ramp_set.argtypes = [C.c_float] * 6
        cls.lib.host_ramp_duration.argtypes = [C.c_float, C.c_float]
        cls.lib.host_ramp_duration.restype = C.c_float
        cls.lib.host_ramp_velocity.argtypes = [C.c_float, C.c_float, C.c_float]
        cls.lib.host_ramp_velocity.restype = C.c_float

    def set_profile(self, p):
        self.lib.host_ramp_set(*p)

    def dur(self, d, s):
        return self.lib.host_ramp_duration(d, s)

    def vel(self, d, t, s):
        return self.lib.host_ramp_velocity(d, t, s)

    # ── duration regimes ────────────────────────────────────────────
    def test_zero_distance(self):
        self.set_profile(NRM)
        self.assertEqual(self.dur(0.0, 4.0), 0.0)

    def test_short_move_constant_slow(self):
        self.set_profile(NRM)
        # Below 2° the move runs at 0.8°/s.
        self.assertAlmostEqual(self.dur(1.99, 4.0), 1.99 / 0.8, places=3)

    def test_regime_transitions(self):
        self.set_profile(NRM)
        # A finite positive duration at every threshold (the duration is NOT
        # monotonic: it drops at the 2° regime change, where the move switches
        # from a 0.8°/s crawl to a faster ramp).
        for d in (0.005, 1.99, 2.0, 2.01, 7.99, 8.0, 10.0, 20.0, 40.0):
            t = self.dur(d, 4.0)
            self.assertTrue(math.isfinite(t), d)
            self.assertGreater(t, 0.0, d)

    def test_minimum_speed_singular_case(self):
        # At exactly min speed the log(v/vmin)/(v-vmin) term is 0/0; the analytic
        # limit 1/vmin keeps the duration finite.
        self.set_profile(NRM)
        self.assertAlmostEqual(self.dur(5.0, 0.8), 6.25, places=3)

    # ── velocity profile ────────────────────────────────────────────
    def test_velocity_profile_monotonic(self):
        self.set_profile(NRM)
        # Trapezoidal 40° at 4°/s: rises to 4, cruises, falls to 0.8.
        self.assertAlmostEqual(self.vel(40.0, 0.0, 4.0), 0.8, places=2)   # floor
        self.assertAlmostEqual(self.vel(40.0, 20.0, 4.0), 4.0, places=2)  # cruise
        self.assertAlmostEqual(self.vel(40.0, 40.0, 4.0), 0.8, places=2)  # floor

    def test_zero_window_ramps(self):
        # accel=0, decel=5 (gentle=0 disables the speed cap): velocity starts at
        # the target and decays to the floor.
        self.lib.host_ramp_set(0.8, 0.0, 0.0, 4.0, 0.0, 5.0)
        self.assertAlmostEqual(self.vel(3.0, 0.0, 4.0), 4.0, places=2)
        self.assertAlmostEqual(self.vel(3.0, 3.0, 4.0), 0.8, places=2)
        # accel=5, decel=0: velocity rises from the floor to the target.
        self.lib.host_ramp_set(0.8, 0.0, 0.0, 4.0, 5.0, 0.0)
        self.assertAlmostEqual(self.vel(3.0, 0.0, 4.0), 0.8, places=2)
        self.assertAlmostEqual(self.vel(3.0, 3.0, 4.0), 4.0, places=2)

    # ── non-finite / invalid ──────────────────────────────────────
    def test_non_finite_inputs(self):
        self.set_profile(NRM)
        for bad in (float("nan"), float("inf"), float("-inf")):
            # A non-finite distance or speed is an error (NaN), not a zero move.
            self.assertTrue(math.isnan(self.dur(bad, 4.0)))
            self.assertTrue(math.isnan(self.dur(10.0, bad)))
            self.assertEqual(self.vel(bad, 1.0, 4.0), 0.0)
        # Negative distance/speed are errors; zero distance/speed are valid.
        self.assertTrue(math.isnan(self.dur(-1.0, 4.0)))
        self.assertTrue(math.isnan(self.dur(10.0, -1.0)))
        self.assertEqual(self.dur(0.0, 4.0), 0.0)
        self.assertEqual(self.dur(10.0, 0.0), 0.0)

    # ── concordance: analytic duration vs numerical integration ─────
    def test_duration_matches_velocity_integral(self):
        for profile in (NRM, BFT):
            self.set_profile(profile)
            for d in (1.0, 3.0, 10.0, 25.0, 40.0):
                analytic = self.dur(d, 4.0)
                # Integrate dt = ∫ dx / v(x) with a fine step.
                n = 20000
                integral = 0.0
                for i in range(n):
                    x0 = d * i / n
                    x1 = d * (i + 1) / n
                    v = self.vel(d, (x0 + x1) / 2.0, 4.0)
                    integral += (x1 - x0) / v
                self.assertAlmostEqual(analytic, integral, delta=5e-3,
                                       msg=(profile, d, analytic, integral))


if __name__ == "__main__":
    unittest.main()
