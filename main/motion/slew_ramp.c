/* Motion — slew_ramp.c — shared slew velocity profile and duration estimate. */
#include "slew_ramp.h"

#include <math.h>

/*
 * Velocity percentage (0-100) at a given travelled distance for the
 * distance-based trapezoidal profile: the first accel window is linear
 * acceleration, the last decel window is linear deceleration, and the middle is
 * cruise.  A move shorter than the full accel+decel window uses a triangular
 * profile whose peak sits proportionally to the accel/decel windows.
 */
static float ramp_percent(const SlewProfile *p, float distance_deg, float travelled_deg) {
    float accel = p->accel_distance_deg;
    float decel = p->decel_distance_deg;
    float total = accel + decel;

    if (distance_deg <= total) {
        if (total <= 0.0f) return 100.0f;
        /* A zero window degenerates to a single-phase ramp. */
        if (accel <= 0.0f)
            return (distance_deg - travelled_deg) * 100.0f / distance_deg;  /* decel only */
        if (decel <= 0.0f)
            return travelled_deg * 100.0f / distance_deg;  /* accel only */
        float peak = distance_deg * accel / total;
        if (travelled_deg < peak)
            return travelled_deg * 100.0f / peak;
        return (distance_deg - travelled_deg) * 100.0f / (distance_deg - peak);
    }

    if (accel > 0.0f && travelled_deg < accel)
        return travelled_deg * 100.0f / accel;
    if (decel > 0.0f && travelled_deg < distance_deg - decel)
        return 100.0f;
    if (decel > 0.0f)
        return (distance_deg - travelled_deg) * 100.0f / decel;
    return 100.0f;
}

float slew_ramp_velocity(const SlewProfile *p, float distance_deg,
                         float travelled_deg, float target_speed_dps) {
    if (!isfinite(distance_deg) || !isfinite(travelled_deg) || !isfinite(target_speed_dps)) {
        return 0.0f;
    }
    if (target_speed_dps <= 0.0f) {
        return 0.0f;
    }
    if (distance_deg <= 0.0f) {
        return target_speed_dps;
    }

    if (distance_deg < p->short_distance_deg) {
        return p->min_speed_dps;
    }

    float capped = target_speed_dps;
    if (distance_deg < p->gentle_distance_deg) {
        if (capped > p->gentle_max_dps) capped = p->gentle_max_dps;
    }

    float pct = ramp_percent(p, distance_deg, travelled_deg);
    return p->min_speed_dps + (capped - p->min_speed_dps) * pct / 100.0f;
}

float slew_ramp_duration(const SlewProfile *p, float distance_deg, float target_speed_dps) {
    /* Non-finite, negative-distance or negative-speed input is an error, not a
     * zero-duration move: return NaN so a caller can tell the difference.  A
     * zero distance or zero speed is a valid zero-length/no-compensation move
     * and returns 0. */
    if (!isfinite(distance_deg) || !isfinite(target_speed_dps)
        || distance_deg < 0.0f || target_speed_dps < 0.0f) {
        return NAN;
    }
    if (distance_deg == 0.0f || target_speed_dps == 0.0f) {
        return 0.0f;
    }

    float min = p->min_speed_dps;
    if (min <= 0.0f) {
        return NAN;
    }

    float capped = target_speed_dps;
    if (distance_deg < p->short_distance_deg) {
        capped = min;
    } else if (distance_deg < p->gentle_distance_deg) {
        if (capped > p->gentle_max_dps) capped = p->gentle_max_dps;
    }

    if (distance_deg < p->short_distance_deg) {
        return distance_deg / min;
    }

    /* Guard the v == vmin singular case: the analytic limit is 1/vmin. */
    float delta = capped - min;
    float ramp_ln;
    if (fabsf(delta) < 1e-6f) {
        ramp_ln = 1.0f / min;
    } else {
        ramp_ln = log1pf(delta / min) / delta;
    }

    float accel = p->accel_distance_deg;
    float decel = p->decel_distance_deg;

    if (distance_deg <= accel + decel) {
        /* Triangular profile (symmetric in every current profile). */
        float result = distance_deg * ramp_ln;
        return isfinite(result) ? result : NAN;
    }

    /* Trapezoidal: accel + cruise + decel, with the two ramp legs summed
     * separately so asymmetric windows are handled correctly. */
    float t_accel = accel * ramp_ln;
    float t_decel = decel * ramp_ln;
    float result = t_accel + t_decel + (distance_deg - accel - decel) / capped;
    return isfinite(result) ? result : NAN;
}
