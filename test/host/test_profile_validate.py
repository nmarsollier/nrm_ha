"""Profile invariant checks — run against the real compiled profile for both mounts.

Replaces the former boot-time ``mount_profile_validate()`` (removed from
production) with direct checks on the compiled profile.  A wrong profile value
fails the host suite — a prerequisite to shipping the firmware — with the
specific field named in the assertion message rather than a generic boolean.
"""
import ctypes as C
import math
import unittest

from harness import Harness


class ProfileSnapshot(C.Structure):
    _fields_ = [
        ("id", C.c_char_p),
        ("name", C.c_char_p),
        ("geometry", C.c_int),
        ("ra_reference_offset_deg", C.c_float),
        ("full_step_deg", C.c_float),
        ("gear_ratio", C.c_float),
        ("canonical_microsteps", C.c_int),
        ("slew_microsteps", C.c_int),
        ("max_slew_dps", C.c_float),
        ("max_manual_dps", C.c_float),
        ("slew_min_speed_dps", C.c_float),
        ("slew_short_distance_deg", C.c_float),
        ("slew_gentle_distance_deg", C.c_float),
        ("slew_gentle_max_dps", C.c_float),
        ("slew_accel_distance_deg", C.c_float),
        ("slew_decel_distance_deg", C.c_float),
        ("ra_hard_min_deg", C.c_float),
        ("ra_hard_max_deg", C.c_float),
        ("ra_default_min_deg", C.c_float),
        ("ra_default_max_deg", C.c_float),
        ("dec_hard_min_deg", C.c_float),
        ("dec_hard_max_deg", C.c_float),
        ("dec_default_min_deg", C.c_float),
        ("dec_default_max_deg", C.c_float),
        ("irun", C.c_int),
        ("ihold", C.c_int),
        ("ra_step_gpio", C.c_int),
        ("ra_dir_gpio", C.c_int),
        ("ra_uart_num", C.c_int),
        ("ra_uart_tx_gpio", C.c_int),
        ("ra_uart_rx_gpio", C.c_int),
        ("dec_step_gpio", C.c_int),
        ("dec_dir_gpio", C.c_int),
        ("dec_uart_num", C.c_int),
        ("dec_uart_tx_gpio", C.c_int),
        ("dec_uart_rx_gpio", C.c_int),
        ("led_gpio", C.c_int),
        ("buzzer_gpio", C.c_int),
    ]


TMC2209_MICROSTEPS = (1, 2, 4, 8, 16, 32, 64, 128, 256)

# ESP32-S3 pins that must not be used for STEP/DIR/UART/LED/buzzer (boot/strapping
# and the UART0 console).
ESP32S3_RESTRICTED = {0, 3, 43, 44, 45, 46}


class ProfileInvariantTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.h = Harness()
        cls.h.lib.host_profile_snapshot.restype = C.POINTER(ProfileSnapshot)
        cls.p = cls.h.lib.host_profile_snapshot().contents

    def test_identity_non_empty(self):
        self.assertTrue(self.p.id, "id must be non-empty")
        self.assertTrue(self.p.name, "name must be non-empty")

    def test_geometry_valid(self):
        self.assertIn(self.p.geometry, (0, 1),
                      "geometry must be GEOMETRY_GERMAN(0) or GEOMETRY_FORK(1)")

    def test_ra_offset_finite(self):
        self.assertTrue(math.isfinite(self.p.ra_reference_offset_deg),
                        "ra_reference_offset_deg must be finite")

    def test_step_angle_positive_finite(self):
        self.assertTrue(math.isfinite(self.p.full_step_deg) and self.p.full_step_deg > 0,
                        "full_step_deg must be positive and finite")

    def test_reduction_positive_finite(self):
        self.assertTrue(math.isfinite(self.p.gear_ratio) and self.p.gear_ratio > 0,
                        "gear_ratio must be positive and finite")

    def test_microsteps_tmc2209(self):
        self.assertIn(self.p.canonical_microsteps, TMC2209_MICROSTEPS,
                      "canonical_microsteps is not a TMC2209 MRES value")
        self.assertIn(self.p.slew_microsteps, TMC2209_MICROSTEPS,
                      "slew_microsteps is not a TMC2209 MRES value")

    def test_canonical_multiple_of_slew(self):
        self.assertEqual(self.p.canonical_microsteps % self.p.slew_microsteps, 0,
                         "canonical_microsteps must be a whole multiple of slew_microsteps")

    def test_speeds_positive_finite(self):
        self.assertTrue(math.isfinite(self.p.max_slew_dps) and self.p.max_slew_dps > 0,
                        "max_slew_dps must be positive and finite")
        self.assertTrue(math.isfinite(self.p.max_manual_dps) and self.p.max_manual_dps > 0,
                        "max_manual_dps must be positive and finite")

    def test_slew_ramp_valid(self):
        self.assertTrue(
            math.isfinite(self.p.slew_min_speed_dps) and self.p.slew_min_speed_dps > 0,
            "slew.min_speed_dps must be positive and finite")
        for name in ("slew_short_distance_deg", "slew_gentle_distance_deg",
                     "slew_accel_distance_deg", "slew_decel_distance_deg"):
            v = getattr(self.p, name)
            self.assertTrue(math.isfinite(v) and v >= 0,
                            f"{name} must be non-negative and finite")
        self.assertTrue(
            math.isfinite(self.p.slew_gentle_max_dps) and self.p.slew_gentle_max_dps > 0,
            "slew.gentle_max_dps must be positive and finite")

    def test_mechanical_limits_ordered(self):
        self.assertTrue(
            math.isfinite(self.p.ra_hard_min_deg) and math.isfinite(self.p.ra_hard_max_deg)
            and self.p.ra_hard_min_deg < self.p.ra_hard_max_deg,
            "RA hard limits must be finite and ordered (min < max)")
        self.assertTrue(
            math.isfinite(self.p.dec_hard_min_deg) and math.isfinite(self.p.dec_hard_max_deg)
            and self.p.dec_hard_min_deg < self.p.dec_hard_max_deg,
            "DEC hard limits must be finite and ordered (min < max)")

    def test_operational_limits_within_mechanical(self):
        self.assertGreaterEqual(self.p.ra_default_min_deg, self.p.ra_hard_min_deg,
                                "ra_default_min_deg outside the RA hard envelope")
        self.assertLessEqual(self.p.ra_default_max_deg, self.p.ra_hard_max_deg,
                             "ra_default_max_deg outside the RA hard envelope")
        self.assertLess(self.p.ra_default_min_deg, self.p.ra_default_max_deg,
                        "RA default limits are not ordered (min < max)")
        self.assertGreaterEqual(self.p.dec_default_min_deg, self.p.dec_hard_min_deg,
                                "dec_default_min_deg outside the DEC hard envelope")
        self.assertLessEqual(self.p.dec_default_max_deg, self.p.dec_hard_max_deg,
                             "dec_default_max_deg outside the DEC hard envelope")
        self.assertLess(self.p.dec_default_min_deg, self.p.dec_default_max_deg,
                        "DEC default limits are not ordered (min < max)")

    def test_irun_ihold_in_range(self):
        self.assertLessEqual(self.p.irun, 31, "irun exceeds the TMC2209 5-bit IHOLD_IRUN field")
        self.assertLessEqual(self.p.ihold, 31, "ihold exceeds the TMC2209 5-bit IHOLD_IRUN field")

    def test_gpio_valid_and_distinct(self):
        gpios = {
            "ra_step_gpio": self.p.ra_step_gpio,
            "ra_dir_gpio": self.p.ra_dir_gpio,
            "ra_uart_tx_gpio": self.p.ra_uart_tx_gpio,
            "ra_uart_rx_gpio": self.p.ra_uart_rx_gpio,
            "dec_step_gpio": self.p.dec_step_gpio,
            "dec_dir_gpio": self.p.dec_dir_gpio,
            "dec_uart_tx_gpio": self.p.dec_uart_tx_gpio,
            "dec_uart_rx_gpio": self.p.dec_uart_rx_gpio,
            "led_gpio": self.p.led_gpio,
            "buzzer_gpio": self.p.buzzer_gpio,
        }
        for name, gpio in gpios.items():
            self.assertGreaterEqual(gpio, 0, f"{name} is negative")
            self.assertLessEqual(gpio, 46, f"{name} is not a valid ESP32-S3 GPIO")
            self.assertNotIn(gpio, ESP32S3_RESTRICTED,
                             f"{name} is a restricted (boot/strapping) pin")
        values = list(gpios.values())
        self.assertEqual(len(set(values)), len(values),
                         "all GPIOs must be distinct (no conflicts)")
        self.assertNotEqual(self.p.led_gpio, self.p.buzzer_gpio,
                            "led_gpio and buzzer_gpio must differ")

    def test_uart_coherent(self):
        self.assertIn(self.p.ra_uart_num, (0, 1, 2), "RA uart_num is invalid")
        self.assertIn(self.p.dec_uart_num, (0, 1, 2), "DEC uart_num is invalid")
        self.assertNotEqual(self.p.ra_uart_num, self.p.dec_uart_num,
                            "RA and DEC must use different UARTs")
