"""Profile validator tests (mount_profile_validate).

Compiles the validator and the NRM profile standalone and verifies the validator
accepts the real profile and rejects mutated copies — in particular non-finite
default limits, which would otherwise slip past every ordering comparison.
"""
import ctypes as C
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
PROFILE = os.environ.get("NRM_TEST_PROFILE", "nrm_ha")

CHECKS = [
    "host_profile_valid",
    "host_reject_nan_dec_min",
    "host_reject_nan_dec_max",
    "host_reject_nan_ra_min",
    "host_reject_nan_ra_max",
    "host_reject_bad_microsteps",
    "host_reject_unordered_limits",
    "host_reject_zero_gear",
    "host_reject_default_outside",
    "host_reject_noncanonical_slew",
]


class ProfileValidateTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        lib = Path(cls.tmp.name) / "profile_validate.so"
        subprocess.run([
            "cc", "-shared", "-fPIC", "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-I" + str(ROOT / "main/profiles"),
            str(ROOT / "main/profiles/mount_profile_validate.c"),
            str(ROOT / f"main/profiles/profile_{PROFILE}.c"),
            str(ROOT / "test/host/profile_validate_driver.c"),
            "-lm", "-o", str(lib),
        ], check=True)
        cls.lib = C.CDLL(str(lib))
        for name in CHECKS:
            fn = getattr(cls.lib, name)
            fn.restype = C.c_bool

    def _check(self, name):
        return bool(getattr(self.lib, name)())

    def test_real_profile_validates(self):
        self.assertTrue(self._check("host_profile_valid"))

    def test_nan_default_limits_rejected(self):
        for name in ("host_reject_nan_dec_min", "host_reject_nan_dec_max",
                     "host_reject_nan_ra_min", "host_reject_nan_ra_max"):
            self.assertTrue(self._check(name), name)

    def test_other_invariants_rejected(self):
        for name in ("host_reject_bad_microsteps", "host_reject_unordered_limits",
                     "host_reject_zero_gear", "host_reject_default_outside",
                     "host_reject_noncanonical_slew"):
            self.assertTrue(self._check(name), name)
