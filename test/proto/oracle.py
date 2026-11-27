"""Independent oracle — geometry and astronomy without reusing the firmware.

Spherical angular distance between equatorial coordinates, handling the RA
24 h / 0 h wrap and careful with polar singularities. RA in hours; DEC in degrees.

Also the slew-time model: a closed-form replica of the firmware ramp profile so a
test can predict how long a goto must take from the observed axis distance.  All
mechanical ramp parameters are passed in explicitly — the oracle keeps no
per-mount constants; the caller supplies the CAPABILITIES ``slew`` ramp.
"""
import math


def capped_speed(speed_dps, distance_deg, min_speed_dps, short_distance_deg,
                 gentle_distance_deg, gentle_max_dps):
    """Target speed after the gentle-slew cap (motors ramp_velocity)."""
    if distance_deg < short_distance_deg:
        return min_speed_dps
    if distance_deg < gentle_distance_deg:
        return min(speed_dps, gentle_max_dps)
    return speed_dps


def slew_time(distance_deg, speed_dps, ramp):
    """Exact single-axis slew duration (s) under the ramp profile `ramp`.

    Velocity ramps linearly with *distance* from ``min_speed_dps`` up to the
    capped target and back down to ``min_speed_dps`` — a trapezoid, or a triangle
    when the move is shorter than the accel+decel window.  Below
    ``short_distance_deg`` the axis runs at constant ``min_speed_dps``.
    dt = ∫ dx / v(x) has the closed form below.

    `ramp` is the CAPABILITIES ``slew`` object: min_speed_dps,
    short_distance_deg, gentle_distance_deg, gentle_max_dps, accel_distance_deg,
    decel_distance_deg.
    """
    if distance_deg <= 0.0:
        return 0.0
    min_speed = ramp["min_speed_dps"]
    short = ramp["short_distance_deg"]
    accel = ramp["accel_distance_deg"]
    decel = ramp["decel_distance_deg"]
    capped = capped_speed(speed_dps, distance_deg, min_speed, short,
                          ramp["gentle_distance_deg"], ramp["gentle_max_dps"])
    if distance_deg < short:
        return distance_deg / min_speed
    ramp_ln = math.log(capped / min_speed) / (capped - min_speed)
    if distance_deg <= accel + decel:  # triangular
        return distance_deg * ramp_ln
    t_accel = accel * ramp_ln
    t_decel = decel * ramp_ln
    return t_accel + t_decel + (distance_deg - accel - decel) / capped


def goto_time(ra_dist_deg, dec_dist_deg, speed_dps, ramp):
    """Duration of a two-axis slew: the slower axis (larger distance) wins."""
    return max(slew_time(ra_dist_deg, speed_dps, ramp),
               slew_time(dec_dist_deg, speed_dps, ramp))


def angular_separation_deg(ra1_h, dec1_deg, ra2_h, dec2_deg):
    """Spherical angular distance (degrees) between two equatorial positions.

    Uses unit vectors to be correct near the pole, where a direct RA
    subtraction loses meaning.
    """
    ra1 = math.radians((ra1_h % 24.0) * 15.0)
    ra2 = math.radians((ra2_h % 24.0) * 15.0)
    d1 = math.radians(dec1_deg)
    d2 = math.radians(dec2_deg)

    x1 = math.cos(d1) * math.cos(ra1)
    y1 = math.cos(d1) * math.sin(ra1)
    z1 = math.sin(d1)
    x2 = math.cos(d2) * math.cos(ra2)
    y2 = math.cos(d2) * math.sin(ra2)
    z2 = math.sin(d2)

    dot = x1 * x2 + y1 * y2 + z1 * z2
    dot = max(-1.0, min(1.0, dot))
    return math.degrees(math.acos(dot))


def normalize_hours(h):
    """Normaliza RA a [0, 24)."""
    return h % 24.0


def wrap_ha(h):
    """Wrap an hour angle to [-12, +12)."""
    h = h % 24.0
    if h >= 12.0:
        h -= 24.0
    return h
