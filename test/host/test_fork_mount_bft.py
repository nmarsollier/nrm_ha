"""Fork (BFT) mount-layer tests — GoTo and PulseGuide with the fork profile.

Compiles the shared mount layer against geometry_fork.c + profile_bft.c and
verifies the profile-specific behaviours: an explicit meridian-flip request is
rejected and the DEC guide sign follows the hemisphere.
"""
import ctypes as C
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
NOW = 1780000000
LAT_SOUTH = -32.89


class ForkMountBftTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        lib = Path(cls.tmp.name) / "fork_mount.so"
        subprocess.run([
            "cc", "-shared", "-fPIC", "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-Dgettimeofday=host_gettimeofday", "-Dtime=host_time",
            "-I" + str(ROOT / "test/host/stubs"), "-I" + str(ROOT / "main"),
            "-I" + str(ROOT / "main/motors"), "-I" + str(ROOT / "main/profiles"), "-I" + str(ROOT / "main/motion"),
            str(ROOT / "main/motion/axis_units.c"),
            str(ROOT / "main/motion/slew_ramp.c"),
            str(ROOT / "main/geometry/geometry_common.c"),
            str(ROOT / "main/geometry/geometry_fork.c"),
            str(ROOT / "main/profiles/profile_bft.c"),
            str(ROOT / "main/mount/mount_coordinate_tools.c"),
            str(ROOT / "main/mount/mount_get_lst.c"),
            str(ROOT / "main/mount/mount_pulse_guide.c"),
            str(ROOT / "main/mount/mount_slew_to_coordinates.c"),
            str(ROOT / "main/mount/mount_destination_side_of_pier.c"),
            str(ROOT / "test/host/mount_stubs.c"), "-lm", "-o", str(lib),
        ], check=True)
        cls.lib = C.CDLL(str(lib))
        cls.lib.host_reset.argtypes = [C.c_long]
        cls.lib.host_lat.argtypes = [C.c_float]
        cls.lib.host_lon.argtypes = [C.c_float]
        cls.lib.host_limits.argtypes = [C.c_float] * 4
        cls.lib.host_dec.argtypes = [C.c_float]
        cls.lib.host_slew.argtypes = [C.c_float, C.c_float, C.c_int]
        cls.lib.host_slew.restype = C.c_bool
        cls.lib.host_slew_side.argtypes = [C.c_float, C.c_float, C.c_int, C.c_int]
        cls.lib.host_slew_side.restype = C.c_bool
        cls.lib.host_slew_called.argtypes = []
        cls.lib.host_slew_called.restype = C.c_int
        cls.lib.host_guide.argtypes = [C.c_int]
        cls.lib.host_guide.restype = C.c_bool
        cls.lib.host_guide_offset.argtypes = []
        cls.lib.host_guide_offset.restype = C.c_float
        cls.lib.mount_set_guide_rate_dec.argtypes = [C.c_float]
        cls.lib.mount_set_guide_rate_dec.restype = C.c_bool
        cls.lib.host_slew_ra.argtypes = []
        cls.lib.host_slew_ra.restype = C.c_float
        cls.lib.host_slew_dec.argtypes = []
        cls.lib.host_slew_dec.restype = C.c_float
        cls.lib.host_guide_axis.argtypes = []
        cls.lib.host_guide_axis.restype = C.c_int

        jd = NOW / 86400 + 2440587.5
        t = (jd - 2451545) / 36525
        cls.lst = (280.46061837 + 360.98564736629 * (jd - 2451545)
                   + .000387933 * t * t - t * t * t / 38710000) % 360 / 15

    def setUp(self):
        self.lib.host_reset(NOW)
        self.lib.host_lat(LAT_SOUTH)
        self.lib.host_lon(0.0)
        self.lib.host_limits(-100, 100, -135, 50)

    # ── GoTo ─────────────────────────────────────────────────────────
    def test_explicit_meridian_flip_rejected(self):
        # PIER_SIDE_EAST = 0, PIER_SIDE_WEST = 1: an explicit side request is
        # not applicable to a fork and must be rejected.
        self.assertFalse(self.lib.host_slew_side(self.lst, -60.0, 4, 0))
        self.assertFalse(self.lib.host_slew_side(self.lst, -60.0, 4, 1))

    def test_goto_auto_accepted_outside_pole(self):
        # A non-pole reachable target (DEC=-60, HA=0 -> R≈0, D=30) is accepted
        # at a real slew speed, exercising the arrival compensation path.  The
        # coordinates sent to the motors are checked, not just the acceptance.
        self.assertTrue(self.lib.host_slew(self.lst, -60.0, 4))
        self.assertEqual(self.lib.host_slew_called(), 1)
        self.assertAlmostEqual(self.lib.host_slew_dec(), 30.0, delta=0.5)
        self.assertLess(abs(self.lib.host_slew_ra()), 1.0)

    # ── PulseGuide ───────────────────────────────────────────────────
    def test_pulse_guide_dec_sign_four_combinations(self):
        # The DEC derivative sign depends on the hemisphere AND on sign(sin D):
        #   north_sign = (sin(D) >= 0 ? +1 : -1) * (-p).
        cases = [
            (-32.89,  30.0, +1.0),   # south, sin(D) > 0
            (-32.89, -30.0, -1.0),   # south, sin(D) < 0
            ( 33.0,   30.0, -1.0),   # north, sin(D) > 0
            ( 33.0,  -30.0, +1.0),   # north, sin(D) < 0
        ]
        for lat, dec, sign in cases:
            self.lib.host_lat(lat)
            self.lib.host_dec(dec)
            self.assertTrue(self.lib.host_guide(0), (lat, dec))
            offset = self.lib.host_guide_offset()
            if sign > 0:
                self.assertGreater(offset, 0.0, (lat, dec))
            else:
                self.assertLess(offset, 0.0, (lat, dec))

    def test_pulse_guide_pole_crossing_rejected(self):
        # A large SOUTH pulse starting near the reference pole (D=0) crosses it
        # and must be rejected before any enqueue.
        self.lib.mount_set_guide_rate_dec(1.0)
        self.lib.host_dec(0.1)
        self.assertFalse(self.lib.host_guide(1))         # SOUTH crosses D=0
        self.assertEqual(self.lib.host_guide_axis(), -1)  # no command enqueued
        self.assertEqual(self.lib.host_guide_offset(), 0.0)  # no setpoint change


if __name__ == "__main__":
    unittest.main()
