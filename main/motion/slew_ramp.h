#pragma once

#include "mount_profile.h"   /* SlewProfile */

/*
 * Slew ramp — the single implementation of the distance-based trapezoidal
 * velocity profile and its closed-form duration integral.
 *
 * Both the motion executor (velocity per batch) and the arrival-time estimator
 * (duration) consume these pure functions, so the predicted goto duration
 * matches the real velocity profile.
 */

/* Effective velocity (deg/s) at `travelled_deg` into a single-axis move of
 * `distance_deg`, targeting `target_speed_dps`.  Returns 0 for non-finite
 * inputs or a non-positive target speed. */
float slew_ramp_velocity(const SlewProfile *p, float distance_deg,
                         float travelled_deg, float target_speed_dps);

/* Estimated duration (seconds) of a single-axis move of `distance_deg` at
 * `target_speed_dps`.  Returns NaN for invalid inputs (non-finite, negative
 * distance or speed, or a non-finite closed-form integral); returns 0 for a
 * zero-length move or a zero speed (no arrival compensation). */
float slew_ramp_duration(const SlewProfile *p, float distance_deg,
                         float target_speed_dps);
