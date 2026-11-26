/* Host — driver for the shared slew-ramp module tests.
 *
 * Compiles slew_ramp.c standalone and exposes a settable SlewProfile plus the
 * pure velocity and duration functions, so the Python tests can exercise the
 * ramp thresholds, the singular case, zero windows and the estimator/executor
 * concordance without RMT or FreeRTOS.
 */
#include "motion/slew_ramp.h"

static SlewProfile g_profile;

void host_ramp_set(float min, float short_d, float gentle, float gentle_max,
                   float accel, float decel) {
    g_profile.min_speed_dps = min;
    g_profile.short_distance_deg = short_d;
    g_profile.gentle_distance_deg = gentle;
    g_profile.gentle_max_dps = gentle_max;
    g_profile.accel_distance_deg = accel;
    g_profile.decel_distance_deg = decel;
}

float host_ramp_duration(float distance, float speed) {
    return slew_ramp_duration(&g_profile, distance, speed);
}

float host_ramp_velocity(float distance, float travelled, float speed) {
    return slew_ramp_velocity(&g_profile, distance, travelled, speed);
}
