#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * Mechanical unit conversion — degrees <-> canonical microsteps.
 *
 * Both axes of a mount share one scale (full-step angle, reduction, canonical
 * microsteps), so there is no per-axis parameter.  The single implementation of
 * the scale and the round-to-nearest conversion is shared by geometry (double)
 * and motors (float wrappers), so both layers agree on quantization near the
 * mechanical limits.
 */

/* Degrees per canonical microstep. */
double axis_deg_per_step(void);

/* Convert a signed angle in degrees to the nearest canonical microstep count.
 * Returns false (and leaves *out_steps untouched) for non-finite input, an
 * out-of-range result or a null output pointer; true otherwise. */
bool axis_deg_to_steps(double degrees, int64_t *out_steps);

/* Angle in degrees for a canonical microstep count. */
double axis_steps_to_deg(int64_t steps);
