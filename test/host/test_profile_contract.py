"""Profile contract tests — run for both profiles.

Verifies, against INDEPENDENT expected values, that the compiled profile has the
declared mechanical scale and DIR polarity, and — for an incomplete profile —
that every motion command is refused and no STEP is emitted.
"""
import ctypes as C
import unittest

from harness import Harness, PROFILE

# Independent expectations per profile — NOT read from the firmware, so a wrong
# reduction or polarity cannot silently shift the assertions.
EXPECTED = {
    "nrm_ha": {"gear": 300, "dir_positive": 1},
    "bft": {"gear": 500, "dir_positive": 0},
}


class ProfileContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.exp = EXPECTED[PROFILE]
        cls.h = Harness()
        cls.h.lib.host_deg_per_step.restype = C.c_float
        cls.h.lib.host_dir_positive_level.argtypes = [C.c_int]
        cls.h.lib.host_dir_positive_level.restype = C.c_int

    def test_compiled_scale_matches_declared_reduction(self):
        expected = 1.8 / (64 * self.exp["gear"])
        self.assertAlmostEqual(self.h.lib.host_deg_per_step(), expected, places=6)

    def test_dir_polarity_matches_declared(self):
        for axis in (0, 1):
            self.assertEqual(self.h.lib.host_dir_positive_level(axis),
                             self.exp["dir_positive"])

    @unittest.skipUnless(PROFILE == "bft", "only the incomplete BFT profile refuses motion")
    def test_incomplete_profile_refuses_motion(self):
        self.h.reset()
        self.assertEqual(self.h.lib.host_mount_guide(0, 500), 0)
        self.assertEqual(self.h.lib.host_mount_tracking(1), 0)
        self.assertEqual(self.h.lib.host_mount_move_axis_speed(0.1, 0.0), 0)
        self.assertEqual(self.h.lib.host_step_count(), 0)
