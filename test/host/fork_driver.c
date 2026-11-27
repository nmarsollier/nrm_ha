/* Host — driver for the fork geometry tests.
 *
 * Compiles geometry_fork.c + geometry_common.c + profile_bft.c and exposes the
 * pure conversions with a raw (lat, lon, time) ABI and settable limits, so the
 * Python tests can build a GeometryContext without touching the C struct layout.
 */
#include "geometry/mount_geometry.h"

#include <stdbool.h>

#include "mount_profile.h"
#include "motion/slew_ramp.h"

static float g_ra_min = -100.0f, g_ra_max = 100.0f;
static float g_dec_min = -135.0f, g_dec_max = 50.0f;

void host_fork_limits(float ra_min, float ra_max, float dec_min, float dec_max) {
    g_ra_min = ra_min; g_ra_max = ra_max;
    g_dec_min = dec_min; g_dec_max = dec_max;
}

static GeometryContext make_ctx(float lat, float lon, double time) {
    GeometryContext ctx = {
        .lat_deg = lat,
        .lon_deg = lon,
        .unix_seconds = time,
        .limits = { g_ra_min, g_ra_max, g_dec_min, g_dec_max },
    };
    return ctx;
}

/* axis -> equatorial. */
void host_fork_forward(float lat, float lon, double time,
                       float ra_axis, float dec_axis,
                       float *out_ra, float *out_dec) {
    GeometryContext ctx = make_ctx(lat, lon, time);
    AxisCoordinates axis = { .ra_axis_deg = ra_axis, .dec_axis_deg = dec_axis };
    EquatorialCoordinates eq = geometry_axis_to_equatorial(&ctx, axis);
    *out_ra = eq.ra_hours;
    *out_dec = eq.dec_deg;
}

/* equatorial -> axis (minimum-travel, no arrival compensation). */
bool host_fork_reverse(float lat, float lon, double time,
                       float ra_hours, float dec_deg,
                       float cur_ra, float cur_dec,
                       float *out_ra, float *out_dec) {
    GeometryContext ctx = make_ctx(lat, lon, time);
    EquatorialCoordinates eq = { .ra_hours = ra_hours, .dec_deg = dec_deg };
    AxisCoordinates current = { .ra_axis_deg = cur_ra, .dec_axis_deg = cur_dec };
    AxisCoordinates out;
    if (!geometry_equatorial_to_axis(&ctx, eq, current, PIER_SIDE_AUTO, 0.0f, &out)) {
        return false;
    }
    *out_ra = out.ra_axis_deg;
    *out_dec = out.dec_axis_deg;
    return true;
}

/* Expose the pure helpers for direct unit checks. */
float host_fork_slew_duration(float distance, float speed) {
    return slew_ramp_duration(&mount_profile.slew, distance, speed);
}
