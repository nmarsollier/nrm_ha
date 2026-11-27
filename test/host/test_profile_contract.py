"""Profile contract tests — run for both profiles.

Verifies, against INDEPENDENT expected values, that the compiled profile has the
declared mechanical scale and DIR polarity.
"""
import ctypes as C
import unittest

from harness import Harness, PROFILE

# Independent expectations per profile — NOT read from the firmware, so a wrong
# reduction, polarity, speed or limit cannot silently shift the assertions.
EXPECTED = {
    "nrm_ha": {"gear": 300, "dir_positive": 1,
               "max_slew": 6.0, "max_manual": 1.0,
               "ra_hard": (-180.0, 180.0), "dec_hard": (-180.0, 180.0)},
    "bft": {"gear": 500, "dir_positive": 0,
            "max_slew": 4.0, "max_manual": 1.0,
            "ra_hard": (-100.0, 100.0), "dec_hard": (-135.0, 50.0)},
}


class ProfileContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.exp = EXPECTED[PROFILE]
        cls.h = Harness()
        cls.h.lib.host_deg_per_step.restype = C.c_float
        cls.h.lib.host_dir_positive_level.argtypes = [C.c_int]
        cls.h.lib.host_dir_positive_level.restype = C.c_int
        cls.h.lib.host_max_slew_dps.restype = C.c_float
        cls.h.lib.host_max_manual_dps.restype = C.c_float
        for name in ("host_hard_min_deg", "host_hard_max_deg"):
            fn = getattr(cls.h.lib, name)
            fn.argtypes = [C.c_int]
            fn.restype = C.c_float

    def test_compiled_scale_matches_declared_reduction(self):
        expected = 1.8 / (64 * self.exp["gear"])
        self.assertAlmostEqual(self.h.lib.host_deg_per_step(), expected, places=6)

    def test_dir_polarity_matches_declared(self):
        for axis in (0, 1):
            self.assertEqual(self.h.lib.host_dir_positive_level(axis),
                             self.exp["dir_positive"])

    def test_max_speeds_match_declared(self):
        self.assertAlmostEqual(self.h.lib.host_max_slew_dps(),
                               self.exp["max_slew"], places=6)
        self.assertAlmostEqual(self.h.lib.host_max_manual_dps(),
                               self.exp["max_manual"], places=6)

    def test_hard_limits_match_declared(self):
        for axis, (lo, hi) in enumerate((self.exp["ra_hard"], self.exp["dec_hard"])):
            self.assertAlmostEqual(self.h.lib.host_hard_min_deg(axis), lo, places=6)
            self.assertAlmostEqual(self.h.lib.host_hard_max_deg(axis), hi, places=6)
