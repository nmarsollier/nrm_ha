"""Fork (BFT) geometry tests — run against geometry_fork.c.

Verifies the pure fork conversions (axis <-> equatorial) against an independent
vector oracle, in both hemispheres, including minimum-travel selection, poles
and limits.  Ported from the BFT reference test_fork.py.
"""
import ctypes as C
import math
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
NOW = 1780000000
LAT_SOUTH = -32.89
LAT_NORTH = 33.0


class ForkBftTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        lib = Path(cls.tmp.name) / "fork.so"
        subprocess.run([
            "cc", "-shared", "-fPIC", "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-I" + str(ROOT / "main"), "-I" + str(ROOT / "main/profiles"), "-I" + str(ROOT / "main/motion"),
            str(ROOT / "main/motion/axis_units.c"),
            str(ROOT / "main/motion/slew_ramp.c"),
            str(ROOT / "main/geometry/geometry_common.c"),
            str(ROOT / "main/geometry/geometry_fork.c"),
            str(ROOT / "main/profiles/profile_bft.c"),
            str(ROOT / "test/host/fork_driver.c"),
            "-lm", "-o", str(lib),
        ], check=True)
        cls.lib = C.CDLL(str(lib))
        cls.lib.host_fork_forward.argtypes = [C.c_float, C.c_float, C.c_double,
                                              C.c_float, C.c_float,
                                              C.POINTER(C.c_float),
                                              C.POINTER(C.c_float)]
        cls.lib.host_fork_reverse.argtypes = [C.c_float, C.c_float, C.c_double,
                                              C.c_float, C.c_float,
                                              C.c_float, C.c_float,
                                              C.POINTER(C.c_float),
                                              C.POINTER(C.c_float)]
        cls.lib.host_fork_reverse.restype = C.c_bool
        cls.lib.host_fork_limits.argtypes = [C.c_float] * 4
        cls.lib.host_fork_slew_duration.argtypes = [C.c_float, C.c_float]
        cls.lib.host_fork_slew_duration.restype = C.c_float

        # Independent sidereal angle at the fixed UTC instant (USNO formula).
        jd = NOW / 86400 + 2440587.5
        t = (jd - 2451545) / 36525
        cls.lst = (280.46061837 + 360.98564736629 * (jd - 2451545)
                   + .000387933 * t * t - t * t * t / 38710000) % 360 / 15

    def setUp(self):
        self.lib.host_fork_limits(-100, 100, -135, 50)

    # ── helpers ─────────────────────────────────────────────────────
    def forward(self, ra_axis, dec_axis, lat=LAT_SOUTH):
        ra = C.c_float(); dec = C.c_float()
        self.lib.host_fork_forward(lat, 0.0, NOW, ra_axis, dec_axis,
                                   C.byref(ra), C.byref(dec))
        return ra.value, dec.value

    def reverse(self, ra_hours, dec_deg, current, lat=LAT_SOUTH):
        ra = C.c_float(); dec = C.c_float()
        ok = self.lib.host_fork_reverse(lat, 0.0, NOW, ra_hours, dec_deg,
                                        current[0], current[1],
                                        C.byref(ra), C.byref(dec))
        return ok, (ra.value, dec.value)

    def oracle_forward(self, ra_axis, dec_axis, lat=LAT_SOUTH):
        p = 1.0 if lat > 0 else -1.0
        R = math.radians(ra_axis)
        D = math.radians(dec_axis)
        sD = math.sin(D)
        x = sD * math.cos(R)
        y = sD * math.sin(R)
        z = p * math.cos(D)
        dec = math.degrees(math.atan2(z, math.hypot(x, y)))
        if abs(sD) < 1e-6:
            ra = (self.lst - ra_axis / 15.0) % 24.0
        else:
            H = math.degrees(math.atan2(y, x))
            ra = (self.lst - H / 15.0) % 24.0
        return ra, dec

    def ha_ra(self, ha_deg):
        return (self.lst - ha_deg / 15.0) % 24.0

    # ── forward (axis -> equatorial) ────────────────────────────────
    def test_forward_matches_oracle_south(self):
        for ra in [-140, -60, -30, 0, 30, 60, 140]:
            for dec in [-150, -90, -30, -1, 1, 30, 90, 149]:
                got = self.forward(ra, dec, LAT_SOUTH)
                exp = self.oracle_forward(ra, dec, LAT_SOUTH)
                self.assertAlmostEqual(got[0], exp[0], places=3, msg=(ra, dec))
                self.assertAlmostEqual(got[1], exp[1], places=3, msg=(ra, dec))

    def test_forward_matches_oracle_north(self):
        for ra in [-140, -60, 0, 60, 140]:
            for dec in [-150, -30, 0, 30, 90, 149]:
                got = self.forward(ra, dec, LAT_NORTH)
                exp = self.oracle_forward(ra, dec, LAT_NORTH)
                self.assertAlmostEqual(got[0], exp[0], places=3, msg=(ra, dec))
                self.assertAlmostEqual(got[1], exp[1], places=3, msg=(ra, dec))

    def test_forward_known_points(self):
        self.assertAlmostEqual(self.forward(0, 0)[1], -90.0, places=4)      # south pole
        self.assertAlmostEqual(self.forward(0, 0, LAT_NORTH)[1], 90.0, places=4)
        self.assertAlmostEqual(self.forward(0, 90)[1], 0.0, places=4)       # equator
        self.assertAlmostEqual(self.forward(0, 57)[1], -33.0, places=3)     # zenith

    def test_flip_equivalents_point_at_same_sky(self):
        for ra in [-140, -60, 0, 60, 140]:
            for dec in [1, 30, 90, 149]:
                a = self.forward(ra, dec)
                b = self.forward(ra + 180, -dec)
                self.assertAlmostEqual(a[0], b[0], places=3)
                self.assertAlmostEqual(a[1], b[1], places=3)

    # ── reverse (equatorial -> axis): minimum travel ────────────────
    def test_goto_current_object_stays_put(self):
        eq = self.forward(60, -30)
        ok, out = self.reverse(eq[0], eq[1], (60, -30))
        self.assertTrue(ok)
        self.assertAlmostEqual(out[0], 60, places=3)
        self.assertAlmostEqual(out[1], -30, places=3)

    def test_goto_south_pole_keeps_ra(self):
        ok, out = self.reverse(self.ha_ra(0), -90, (75, 30))
        self.assertTrue(ok)
        self.assertAlmostEqual(out[0], 75, places=3)
        self.assertAlmostEqual(out[1], 0, places=3)

    def test_goto_north_pole_not_confused_with_south(self):
        self.lib.host_fork_limits(-200, 200, -200, 200)
        ok, out = self.reverse(self.ha_ra(0), 90, (40, 0))
        self.assertTrue(ok)
        self.assertAlmostEqual(out[0], 40, places=3)
        self.assertAlmostEqual(abs((out[1] - 180) % 360), 0, places=3)

    def test_goto_north_pole_rejected_without_range(self):
        self.lib.host_fork_limits(-150, 150, -150, 150)
        ok, _ = self.reverse(self.ha_ra(0), 90, (40, 0))
        self.assertFalse(ok)

    def test_reject_out_of_limits(self):
        self.lib.host_fork_limits(-90, 90, -20, 120)
        ok, _ = self.reverse(self.ha_ra(160), -60, (0, 0))
        self.assertFalse(ok)

    def test_admit_with_wider_limits(self):
        self.lib.host_fork_limits(-150, 150, -150, 150)
        ok, out = self.reverse(self.ha_ra(160), -60, (0, 0))
        self.assertTrue(ok)
        self.assertAlmostEqual(out[0], -20, places=3)
        self.assertAlmostEqual(out[1], -30, places=3)

    def test_periodicity_prefers_2_degree_travel(self):
        self.lib.host_fork_limits(-180, 181, -150, 150)
        ok, out = self.reverse(self.ha_ra(-179), 0, (179, 0))
        self.assertTrue(ok)
        self.assertAlmostEqual(out[0], 181, places=3)

    def test_invalid_inputs_rejected(self):
        bad = [(math.nan, 0), (math.inf, 0), (0, math.nan), (0, math.inf),
               (0, -91), (0, 91)]
        for ra_h, dec_d in bad:
            ok, _ = self.reverse(ra_h, dec_d, (0, 0))
            self.assertFalse(ok, (ra_h, dec_d))

    # ── shared helpers (slew duration) ──────────────────────────────
    def test_slew_duration_singular_case(self):
        # At exactly min speed the estimator's log(v/vmin)/(v-vmin) is 0/0; it
        # must resolve to the analytic limit 1/vmin, never NaN.
        d = self.lib.host_fork_slew_duration(5.0, 0.8)
        self.assertTrue(math.isfinite(d))
        self.assertGreater(d, 0.0)
        self.assertAlmostEqual(d, 6.25, places=3)   # 5 deg * (1 / 0.8 deg/s)


if __name__ == "__main__":
    unittest.main()
