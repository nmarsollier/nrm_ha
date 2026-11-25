/* Motion — axis_units.c — mechanical unit conversion (per axis). */
#include "axis_units.h"

#include "mount_profile.h"

double axis_deg_per_step(AxisId axis) {
    const AxisProfile *a = &mount_profile.axes[axis];
    return (double) a->full_step_deg
           / ((double) a->canonical_microsteps * (double) a->gear_ratio);
}

int64_t axis_deg_to_steps(AxisId axis, double degrees) {
    double step = axis_deg_per_step(axis);
    return (int64_t)(degrees / step + (degrees >= 0.0 ? 0.5 : -0.5));
}

double axis_steps_to_deg(AxisId axis, int64_t steps) {
    return (double) steps * axis_deg_per_step(axis);
}
