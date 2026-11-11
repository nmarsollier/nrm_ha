"""Execute production C coordinate/guide math without touching hardware.

The mount coordinate conversion (mount_coordinate_tools.c) is a pure function of
(position, lon, time): the two pier-side solutions for a German equatorial are
computed and the min-travel one is selected.  These tests compile that file —
plus the LST, pulse-guide and slew-to-coordinates cases — with the motors layer
and the clock mocked, and verify:

  - forward (axis -> equatorial) against an independent oracle,
  - reverse (equatorial -> axis) minimum-travel selection and round-trips,
  - flip symmetry (both pier sides point at the same sky),
  - pole handling (DEC = -90 at dec_axis = 0),
  - guiding signs derived from the current mechanical DEC sign,
  - GOTO accept/reject and destination within limits.
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
LON = 0.0
HOME_RA_OFFSET_DEG = 90.0


class Axis(C.Structure):
    _fields_ = [("ra_axis_deg", C.c_float), ("dec_axis_deg", C.c_float),
                ("pier_side", C.c_int)]


class Eq(C.Structure):
    _fields_ = [("ra_hours", C.c_float), ("dec_deg", C.c_float)]


def _norm_deg(d):
    d = d % 360.0
    if d >= 180.0:
        d -= 360.0
    if d < -180.0:
        d += 360.0
    return d


def _norm_hours(h):
    while h < 0.0:
        h += 24.0
    while h >= 24.0:
        h -= 24.0
    return h


class ForkTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        lib = Path(cls.tmp.name) / "mount.so"
        subprocess.run([
            "cc", "-shared", "-fPIC", "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-Dgettimeofday=host_gettimeofday",
            "-Dtime=host_time",
            "-I" + str(ROOT / "test/host/stubs"), "-I" + str(ROOT / "main"),
            "-I" + str(ROOT / "main/motors"),
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
        cls.lib.host_time_valid.argtypes = [C.c_int]
        cls.lib.host_limits.argtypes = [C.c_float] * 4
        cls.lib.host_dec.argtypes = [C.c_float]
        cls.lib.host_ra.argtypes = [C.c_float]
        cls.lib.host_set_time.argtypes = [C.c_double]
        cls.lib.host_slew.argtypes = [C.c_float, C.c_float, C.c_int]
        cls.lib.host_slew.restype = C.c_bool
        cls.lib.axis_to_equatorial.argtypes = [Axis]
        cls.lib.axis_to_equatorial.restype = Eq
        cls.lib.equatorial_to_axis.argtypes = [Eq, Axis, C.c_long, C.c_int, C.POINTER(Axis)]
        cls.lib.equatorial_to_axis.restype = C.c_bool
        cls.lib.host_guide.argtypes = [C.c_int]
        cls.lib.host_guide.restype = C.c_bool
        cls.lib.host_destination_side_of_pier.argtypes = [C.c_float, C.c_float]
        cls.lib.host_destination_side_of_pier.restype = C.c_int

    def setUp(self):
        self.lib.host_reset(NOW)
        # Independent sidereal angle at the fixed UTC instant (USNO formula).
        jd = NOW / 86400 + 2440587.5
        t = (jd - 2451545) / 36525
        self.lst = (280.46061837 + 360.98564736629 * (jd - 2451545)
                    + .000387933 * t * t - t * t * t / 38710000) % 360 / 15

    # ── oracle (independent of the C code under test) ────────────────
    def oracle_forward(self, ra_axis, dec_axis):
        # pierEast (dec_axis >= 0): HA = ra + offset, DEC = dec_axis - 90.
        # pierWest (dec_axis <  0): HA = ra - offset, DEC = -dec_axis - 90.
        if dec_axis >= 0.0:
            ha_deg = _norm_deg(ra_axis + HOME_RA_OFFSET_DEG)
            dec = dec_axis - 90.0
        else:
            ha_deg = _norm_deg(ra_axis - HOME_RA_OFFSET_DEG)
            dec = -dec_axis - 90.0
        ra_hours = _norm_hours(self.lst - ha_deg / 15.0)
        return ra_hours, dec

    def forward(self, ra_axis, dec_axis):
        return self.lib.axis_to_equatorial(Axis(ra_axis, dec_axis, 0))

    def reverse(self, ra_hours, dec_deg, current, required=-1):
        out = Axis(999, 999, 99)
        ok = self.lib.equatorial_to_axis(Eq(ra_hours, dec_deg), current,
                                         NOW, required, C.byref(out))
        return ok, out

    def ha_ra(self, ha_deg):
        """RA hours whose hour angle is ha_deg at the fixed instant."""
        return _norm_hours(self.lst - ha_deg / 15.0)

    # ── forward ─────────────────────────────────────────────────────
    def test_forward_matches_oracle(self):
        for ra in [-140, -60, -30, 0, 30, 60, 140]:
            for dec in [-150, -90, -30, -1, 0, 1, 30, 90, 149]:
                got = self.forward(ra, dec)
                era, edec = self.oracle_forward(ra, dec)
                self.assertAlmostEqual(got.ra_hours, era, delta=2e-2)
                self.assertAlmostEqual(got.dec_deg, edec, delta=2e-2)

    def test_forward_known_points(self):
        # dec_axis = 90 is the equator (DEC=0); HA = ra + 90.
        self.assertAlmostEqual(self.forward(0, 90).dec_deg, 0.0, delta=1e-3)
        self.assertAlmostEqual(self.forward(0, 90).ra_hours,
                               _norm_hours(self.lst - 6), delta=1e-3)
        # dec_axis = 0 is the south celestial pole (DEC=-90).
        self.assertAlmostEqual(self.forward(0, 0).dec_deg, -90.0, delta=1e-3)
        # pierWest: dec_axis = -90 is the equator too, HA = ra - 90.
        self.assertAlmostEqual(self.forward(0, -90).dec_deg, 0.0, delta=1e-3)
        self.assertAlmostEqual(self.forward(0, -90).ra_hours,
                               _norm_hours(self.lst + 6), delta=1e-3)
        # ra=30, dec=30 -> DEC=-60, HA=120 (RA = LST - 8).
        got = self.forward(30, 30)
        self.assertAlmostEqual(got.dec_deg, -60.0, delta=1e-3)
        self.assertAlmostEqual(got.ra_hours, _norm_hours(self.lst - 8), delta=1e-3)
        # ra=-30, dec=-30 (pierWest) -> DEC=-60, HA=-120 (RA = LST + 8).
        got = self.forward(-30, -30)
        self.assertAlmostEqual(got.dec_deg, -60.0, delta=1e-3)
        self.assertAlmostEqual(got.ra_hours, _norm_hours(self.lst + 8), delta=1e-3)

    # ── flip symmetry ───────────────────────────────────────────────
    def test_flip_equivalents_point_at_same_sky(self):
        for ra in [-140, -60, 0, 60, 140]:
            for dec in [1, 30, 90, 149]:
                a = self.forward(ra, dec)
                b = self.forward(ra + 180, -dec)
                self.assertAlmostEqual(a.dec_deg, b.dec_deg, delta=1e-3)
                self.assertAlmostEqual(a.ra_hours, b.ra_hours, delta=1e-3)

    # ── reverse (pier-side selection) ───────────────────────────────
    def test_round_trip_recovers_position(self):
        for ra in [-60, -30, 0, 30, 60]:
            for dec in [30, 90, 149]:
                eq = self.forward(ra, dec)
                ok, out = self.reverse(eq.ra_hours, eq.dec_deg, Axis(ra, dec, 0))
                self.assertTrue(ok)
                self.assertAlmostEqual(out.ra_axis_deg, ra, delta=1e-2)
                self.assertAlmostEqual(out.dec_axis_deg, dec, delta=1e-2)

    def _cur(self, side):
        """Representative axis position on the given pier side (0 east, 1 west)."""
        return Axis(0.0, 60.0, 0) if side == 0 else Axis(0.0, -60.0, 1)

    def test_reverse_east_keeps_east_far_west(self):
        # From EAST, a far-west target (HA=+150°) stays EAST (the asymmetric
        # "stick to EAST" policy): pierEast ra=+60, dec=+60.
        ok, out = self.reverse(self.ha_ra(150), -30, Axis(0, 0, 0))
        self.assertTrue(ok)
        self.assertAlmostEqual(out.ra_axis_deg, 60, delta=1e-2)
        self.assertAlmostEqual(out.dec_axis_deg, 60, delta=1e-2)
        self.assertEqual(out.pier_side, 0)

    def test_meridian_flip_policy_matrix(self):
        """Asymmetric AUTO policy keyed on the current side (WEST=1, EAST=0)."""
        self.lib.host_limits(-100, 100, -150, 150)
        cases = [
            # From WEST: flip to EAST exactly at HA=0.
            (1, -1.0,   1),
            (1, -0.01,  1),
            (1,  0.0,   0),
            (1,  0.01,  0),
            (1,  1.0,   0),
            # From EAST: stick to EAST regardless of small HA.
            (0,  1.0,   0),
            (0,  0.01,  0),
            (0,  0.0,   0),
            (0, -0.01,  0),
            (0, -3.0,   0),
            (0, -5.0,   0),
        ]
        for side, ha, want in cases:
            ok, out = self.reverse(self.ha_ra(ha), -30.0, self._cur(side))
            self.assertTrue(ok, f"side={side} HA={ha} must be solvable")
            self.assertEqual(out.pier_side, want,
                             f"side={side} HA={ha}: got {out.pier_side}, want {want}")

    def test_meridian_flip_crosses_to_opposite_side(self):
        """WEST -> EAST at HA=+1° (the flip)."""
        self.lib.host_limits(-100, 100, -150, 150)
        ok, out = self.reverse(self.ha_ra(1.0), -30.0, self._cur(1))
        self.assertTrue(ok)
        self.assertEqual(out.pier_side, 0)
        self.assertAlmostEqual(out.ra_axis_deg, -89.0, delta=1e-2)
        self.assertAlmostEqual(out.dec_axis_deg, 60.0, delta=1e-2)

    def test_meridian_flip_stays_west_east_of_meridian(self):
        """WEST stays WEST at HA=-1° (east of meridian)."""
        self.lib.host_limits(-100, 100, -150, 150)
        ok, out = self.reverse(self.ha_ra(-1.0), -30.0, self._cur(1))
        self.assertTrue(ok)
        self.assertEqual(out.pier_side, 1)
        self.assertAlmostEqual(out.ra_axis_deg, 89.0, delta=1e-2)
        self.assertAlmostEqual(out.dec_axis_deg, -60.0, delta=1e-2)

    def test_meridian_flip_repeated_goto_stays_on_new_side(self):
        """After the flip (EAST), a repeated goto does not revert to WEST."""
        self.lib.host_limits(-100, 100, -150, 150)
        ok, out = self.reverse(self.ha_ra(1.0), -30.0, self._cur(0))
        self.assertTrue(ok)
        self.assertEqual(out.pier_side, 0)
        self.assertAlmostEqual(out.ra_axis_deg, -89.0, delta=1e-2)
        self.assertAlmostEqual(out.dec_axis_deg, 60.0, delta=1e-2)

    def test_meridian_flip_east_sticks_even_slightly_east(self):
        """EAST at HA=-0.01° stays EAST (no snap back across the meridian)."""
        self.lib.host_limits(-100, 100, -150, 150)
        ok, out = self.reverse(self.ha_ra(-0.01), -30.0, self._cur(0))
        self.assertTrue(ok)
        self.assertEqual(out.pier_side, 0)
        self.assertAlmostEqual(out.ra_axis_deg, -90.01, delta=1e-2)
        self.assertAlmostEqual(out.dec_axis_deg, 60.0, delta=1e-2)

    def test_meridian_flip_east_falls_back_when_east_out_of_limits(self):
        """From EAST, go WEST only when EAST is out of the RA limit (HA=-20°)."""
        self.lib.host_limits(-100, 100, -150, 150)
        ok, out = self.reverse(self.ha_ra(-20.0), -30.0, self._cur(0))
        self.assertTrue(ok)
        self.assertEqual(out.pier_side, 1)   # EAST ra=-110 is out; WEST is reachable
        self.assertAlmostEqual(out.ra_axis_deg, 70.0, delta=1e-2)
        self.assertAlmostEqual(out.dec_axis_deg, -60.0, delta=1e-2)

    def test_meridian_flip_west_rejects_when_flip_unreachable(self):
        """From WEST with HA>=0, an unreachable EAST must reject (no fallback)."""
        self.lib.host_limits(-50, 100, -150, 150)   # asymmetric: EAST ra=-90 is out
        ok, _ = self.reverse(self.ha_ra(0.0), -30.0, self._cur(1))
        self.assertFalse(ok)

    def test_meridian_flip_west_ha_negative_falls_back_to_east(self):
        """From WEST with HA<0, an unreachable WEST falls back to EAST.

        The no-fallback rule applies only to the flip (HA>=0); a stay-WEST goto
        whose WEST solution is out of limits may still use EAST.
        """
        self.lib.host_limits(-100, 100, -100, 150)   # DEC=+30 -> WEST dec=-120 out
        ok, out = self.reverse(self.ha_ra(-1.0), 30.0, self._cur(1))
        self.assertTrue(ok)
        self.assertEqual(out.pier_side, 0)           # EAST
        self.assertAlmostEqual(out.ra_axis_deg, -91.0, delta=1e-2)
        self.assertAlmostEqual(out.dec_axis_deg, 120.0, delta=1e-2)

    def test_reverse_invalid_required_rejected(self):
        """A `required` value outside AUTO/EAST/WEST is rejected (no OOB index)."""
        self.lib.host_limits(-100, 100, -150, 150)
        ok, _ = self.reverse(self.ha_ra(0.0), -30.0, Axis(0, 0, 0), required=99)
        self.assertFalse(ok)

    def test_meridian_flip_sequence_west_east_centers(self):
        """WEST -> flip EAST -> several center gotos stay EAST."""
        self.lib.host_limits(-100, 100, -150, 150)
        cur = self._cur(1)                                  # WEST
        ok, out = self.reverse(self.ha_ra(0.5), -30.0, cur)
        self.assertTrue(ok)
        self.assertEqual(out.pier_side, 0)                  # flipped EAST

        cur = out
        ok, out = self.reverse(self.ha_ra(-0.01), -30.0, cur)
        self.assertTrue(ok)
        self.assertEqual(out.pier_side, 0)                  # stays EAST

        cur = out
        ok, out = self.reverse(self.ha_ra(0.01), -30.0, cur)
        self.assertTrue(ok)
        self.assertEqual(out.pier_side, 0)                  # stays EAST

        cur = out
        ok, out = self.reverse(self.ha_ra(-3.0), -30.0, cur)
        self.assertTrue(ok)
        self.assertEqual(out.pier_side, 0)                  # stays EAST

    def test_meridian_flip_normalizes_across_antimeridian(self):
        """HA near ±12h normalises to the HA-correct side from WEST."""
        self.lib.host_limits(-100, 100, -150, 150)
        ok, out = self.reverse(self.ha_ra(179.0), -30.0, self._cur(1))
        self.assertTrue(ok)
        self.assertEqual(out.pier_side, 0)           # HA>=0 -> EAST
        self.assertAlmostEqual(out.ra_axis_deg, 89.0, delta=1e-2)
        self.assertAlmostEqual(out.dec_axis_deg, 60.0, delta=1e-2)

        ok, out = self.reverse(self.ha_ra(-179.0), -30.0, self._cur(1))
        self.assertTrue(ok)
        self.assertEqual(out.pier_side, 1)           # HA<0 -> WEST
        self.assertAlmostEqual(out.ra_axis_deg, -89.0, delta=1e-2)
        self.assertAlmostEqual(out.dec_axis_deg, -60.0, delta=1e-2)

    def test_meridian_flip_pole_target(self):
        """DEC=-90 (the pole) has no DEC component; the side is degenerate."""
        self.lib.host_limits(-100, 100, -150, 150)
        ok, out = self.reverse(self.ha_ra(1.0), -90.0, Axis(0, 0, 0))
        self.assertTrue(ok)
        self.assertAlmostEqual(out.dec_axis_deg, 0.0, delta=1e-2)
        got = self.forward(out.ra_axis_deg, out.dec_axis_deg)
        self.assertAlmostEqual(got.dec_deg, -90.0, delta=1e-2)

    def test_meridian_flip_explicit_side_forces_side(self):
        """An explicit side overrides the hour-angle preference.

        HA=+1° (west) would pick EAST; requesting WEST forces WEST.
        """
        self.lib.host_limits(-100, 100, -150, 150)
        ok, out = self.reverse(self.ha_ra(1.0), -30.0, self._cur(1), required=1)
        self.assertTrue(ok)
        self.assertEqual(out.pier_side, 1)
        self.assertAlmostEqual(out.ra_axis_deg, 91.0, delta=1e-2)
        self.assertAlmostEqual(out.dec_axis_deg, -60.0, delta=1e-2)

    def test_meridian_flip_explicit_east_overrides_auto(self):
        """An explicit EAST request flips even at HA=+0.2° (no deadband)."""
        self.lib.host_limits(-100, 100, -150, 150)
        ok, out = self.reverse(self.ha_ra(0.2), -30.0, self._cur(1), required=0)
        self.assertTrue(ok)
        self.assertEqual(out.pier_side, 0)
        self.assertAlmostEqual(out.ra_axis_deg, -89.8, delta=1e-2)
        self.assertAlmostEqual(out.dec_axis_deg, 60.0, delta=1e-2)

    def test_meridian_flip_explicit_side_unreachable_rejects(self):
        """An explicit side that is out of limits must reject, never fall back.

        HA=+11° leaves WEST (ra=+101) outside the ±100 RA limit.  Requesting
        WEST must fail rather than silently stay on the reachable EAST side.
        """
        self.lib.host_limits(-100, 100, -150, 150)
        ok, _ = self.reverse(self.ha_ra(11.0), -30.0, Axis(0, 0, 0), required=1)
        self.assertFalse(ok)

    def test_goto_locks_side_across_meridian_crossing(self):
        """The destination side is decided once, at the request instant.

        A long slew from WEST to a target at HA=-0.1° drifts HA past zero by the
        arrival instant; the destination must stay WEST (dec_axis < 0) rather
        than flip mid-slew.
        """
        self.lib.host_limits(-100, 100, -150, 150)
        self.lib.host_ra(-90.0)   # WEST, far from the target's WEST solution
        self.lib.host_dec(-60.0)
        ok, _, _, dec = self._slew(self.ha_ra(-0.1), -30.0)
        self.assertTrue(ok)
        self.assertLess(dec, 0.0, "side must stay locked to WEST across the crossing")

    # ── destination-side prediction (read-only) ─────────────────────
    def test_destination_side_matches_goto_selection(self):
        """The read-only prediction uses the same selection as a real goto."""
        self.lib.host_limits(-100, 100, -150, 150)
        for ha in (-5.0, -0.01, 0.0, 0.01, 5.0):
            for side in (0, 1):
                cur = self._cur(side)
                self.lib.host_ra(cur.ra_axis_deg)
                self.lib.host_dec(cur.dec_axis_deg)
                pred = self.lib.host_destination_side_of_pier(self.ha_ra(ha), -30.0)
                ok, out = self.reverse(self.ha_ra(ha), -30.0, cur)
                self.assertTrue(ok)
                self.assertEqual(pred, out.pier_side,
                                 f"HA={ha} side={side}: pred {pred} != goto {out.pier_side}")

    def test_destination_side_is_read_only(self):
        """The prediction enqueues nothing (slew_called stays 0)."""
        self.lib.host_limits(-100, 100, -150, 150)
        self.lib.host_ra(0.0)
        self.lib.host_dec(-60.0)
        self.lib.host_destination_side_of_pier(self.ha_ra(1.0), -30.0)
        self.assertEqual(C.c_int.in_dll(self.lib, "slew_called").value, 0,
                         "prediction must not enqueue a move")

    def test_reverse_falls_back_to_other_side(self):
        # The nearest side is out of DEC limits, so the other side must win.
        # Target HA=+150, DEC=+60 -> pierEast dec = +150 (> limit 100),
        # pierWest dec = -150 (out), with wider DEC: -150..150.
        self.lib.host_limits(-150, 150, -100, 100)
        # pierEast dec = DEC+90 = +150 (out); pierWest dec = -(DEC+90) = -150 (out)
        # -> nothing valid, reject.
        ok, _ = self.reverse(self.ha_ra(150), 60, Axis(0, 0, 0))
        self.assertFalse(ok)

    def test_reverse_rejects_out_of_limits(self):
        # DEC=+60 with dec limits -100..100 has no valid pier side.
        self.lib.host_limits(-150, 150, -100, 100)
        ok, _ = self.reverse(self.ha_ra(0), 60, Axis(0, 0, 0))
        self.assertFalse(ok)

    def test_reverse_invalid_dec_rejected(self):
        for dec in [-91.0, 91.0]:
            ok, _ = self.reverse(self.ha_ra(0), dec, Axis(0, 0, 0))
            self.assertFalse(ok)

    def test_reverse_rejects_non_finite(self):
        # NaN/inf must be rejected before normalization: a +inf RA would
        # otherwise spin the `while` normalization forever.
        for ra, dec in [(float("nan"), 0.0), (float("inf"), 0.0),
                        (float("-inf"), 0.0), (0.0, float("nan")),
                        (0.0, float("inf")), (0.0, float("-inf"))]:
            ok, _ = self.reverse(ra, dec, Axis(0, 0, 0))
            self.assertFalse(ok, f"non-finite ra={ra} dec={dec} must be rejected")

    # ── pole handling ───────────────────────────────────────────────
    def test_pole_maps_to_south_pole(self):
        # dec_axis = 0 (exact pole) reports DEC = -90 regardless of RA.
        self.assertAlmostEqual(self.forward(0, 0).dec_deg, -90.0, delta=1e-3)
        self.assertAlmostEqual(self.forward(75, 0).dec_deg, -90.0, delta=1e-3)

    # ── guiding signs (dec_axis sign, not latitude) ─────────────────
    def _guide(self, direction, dec):
        self.lib.host_dec(dec)
        return self.lib.host_guide(direction)

    def test_guide_ra_signs(self):
        # East -> -RA, West -> +RA (axis 0).
        self.assertTrue(self._guide(2, 30))
        self.assertEqual(C.c_int.in_dll(self.lib, "guide_axis").value, 0)
        self.assertLess(C.c_float.in_dll(self.lib, "guide_offset").value, 0)
        self.assertTrue(self._guide(3, 30))
        self.assertEqual(C.c_int.in_dll(self.lib, "guide_axis").value, 0)
        self.assertGreater(C.c_float.in_dll(self.lib, "guide_offset").value, 0)

    def test_guide_dec_signs(self):
        # North on dec_axis > 0 increases DEC (+), on dec_axis < 0 decreases (-).
        self.assertTrue(self._guide(0, 30))
        self.assertEqual(C.c_int.in_dll(self.lib, "guide_axis").value, 1)
        self.assertGreater(C.c_float.in_dll(self.lib, "guide_offset").value, 0)
        self.assertTrue(self._guide(0, -30))
        self.assertLess(C.c_float.in_dll(self.lib, "guide_offset").value, 0)
        # South is the opposite.
        self.assertTrue(self._guide(1, 30))
        self.assertLess(C.c_float.in_dll(self.lib, "guide_offset").value, 0)
        self.assertTrue(self._guide(1, -30))
        self.assertGreater(C.c_float.in_dll(self.lib, "guide_offset").value, 0)

    # ── GOTO (mount_slew_to_coordinates) ────────────────────────────
    def _slew(self, ra_hours, dec_deg, speed=4):
        ok = self.lib.host_slew(ra_hours, dec_deg, speed)
        called = C.c_int.in_dll(self.lib, "slew_called").value
        ra = C.c_float.in_dll(self.lib, "slew_ra").value
        dec = C.c_float.in_dll(self.lib, "slew_dec").value
        return ok, called, ra, dec

    def test_goto_accepts_valid_target(self):
        self.lib.host_limits(-150, 150, -150, 150)
        ok, called, ra, dec = self._slew(self.ha_ra(0), -60)
        self.assertTrue(ok)
        self.assertEqual(called, 1)
        self.assertTrue(-150 <= ra <= 150, ra)
        self.assertTrue(-150 <= dec <= 150, dec)

    def test_goto_rejects_without_solution(self):
        # DEC=+60 -> dec_base=+150: pierEast dec=+150 (>120) and pierWest
        # dec=-150 (<-20) are both outside the DEC limits, so no side is valid.
        self.lib.host_limits(-90, 90, -20, 120)
        self.lib.host_ra(0)
        self.lib.host_dec(0)
        ok, called, _, _ = self._slew(self.ha_ra(0), 60)
        self.assertFalse(ok)
        self.assertEqual(called, 0)

    def test_goto_rejects_without_time(self):
        self.lib.host_time_valid(0)
        ok, called, _, _ = self._slew(self.ha_ra(0), -60)
        self.assertFalse(ok)
        self.assertEqual(called, 0)

    def test_goto_destination_points_at_target(self):
        # The slew destination, fed back through forward(), lands on the
        # requested sky position (allowing for the small sidereal drift during
        # the arrival compensation).
        self.lib.host_limits(-150, 150, -150, 150)
        target_ha = 0.0
        target_dec = -60.0
        ok, _, ra, dec = self._slew(self.ha_ra(target_ha), target_dec)
        self.assertTrue(ok)
        got = self.forward(ra, dec)
        # forward() reads the NOW clock, but the slew destination is computed
        # for the arrival instant: allow ~1° of sidereal drift.
        target_ra = self.ha_ra(target_ha)
        self.assertAlmostEqual(got.ra_hours, target_ra, delta=1.0)
        self.assertAlmostEqual(got.dec_deg, target_dec, delta=1.0)


if __name__ == "__main__":
    unittest.main()
