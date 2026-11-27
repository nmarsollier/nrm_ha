/* Motion — axis_units.c — mechanical unit conversion. */
#include "axis_units.h"

#include <math.h>
#include <stddef.h>

#include "mount_profile.h"

double axis_deg_per_step(void) {
    return (double) mount_profile.full_step_deg
           / ((double) mount_profile.canonical_microsteps * (double) mount_profile.gear_ratio);
}

bool axis_deg_to_steps(double degrees, int64_t *out_steps) {
    if (out_steps == NULL) {
        return false;
    }
    /* Non-finite input has no representable step count. */
    if (!isfinite(degrees)) {
        return false;
    }
    double step = axis_deg_per_step();
    double steps = degrees / step + (degrees >= 0.0 ? 0.5 : -0.5);
    /* Reject an out-of-range result rather than saturate it: a caller must be
     * able to tell an invalid conversion from a real position. */
    if (steps >= 9223372036854775808.0 || steps < -9223372036854775808.0) {
        return false;   /* overflow */
    }
    *out_steps = (int64_t) steps;
    return true;
}

double axis_steps_to_deg(int64_t steps) {
    return (double) steps * axis_deg_per_step();
}
