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

/* Nearest canonical microstep count for a signed angle in degrees. */
int64_t axis_deg_to_steps(AxisId axis, double degrees);

/* Angle in degrees for a canonical microstep count. */
double axis_steps_to_deg(AxisId axis, int64_t steps);
