#pragma once

#include <stdint.h>

#include "mount_profile.h"   /* AxisId */

/*
 * Mechanical unit conversion — degrees <-> canonical microsteps, per axis.
 *
 * The single implementation of the scale and the round-to-nearest conversion,
 * shared by geometry (double) and motors (float wrappers), so both layers agree
 * on quantization near the mechanical limits.
 */

/* Degrees per canonical microstep for one axis. */
double axis_deg_per_step(AxisId axis);

/* Convert a signed angle in degrees to the nearest canonical microstep count.
 * Returns false (and leaves *out_steps untouched) for non-finite input, an
 * out-of-range result or a null output pointer; true otherwise. */
bool axis_deg_to_steps(AxisId axis, double degrees, int64_t *out_steps);

/* Angle in degrees for a canonical microstep count. */
double axis_steps_to_deg(AxisId axis, int64_t steps);
