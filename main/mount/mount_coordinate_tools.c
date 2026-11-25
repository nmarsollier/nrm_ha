/* Mount - mount_coordinate_tools.c
 *
 * Purpose: adapt the pure geometry (main/geometry/) to the mount's current
 * mutable state — site, mechanical limits and the wall clock.  The coordinate
 * math itself lives in the geometry module; these wrappers only assemble the
 * GeometryContext from the mount state and delegate.
 */
#include "mount.h"
#include "mount_internal.h"

#include <math.h>
#include <stdbool.h>
#include <sys/time.h>
#include <time.h>

#include "geometry/mount_geometry.h"

/* Assemble the immutable geometry context from the current mount state at the
 * given fractional instant. */
static GeometryContext context_at(double unix_seconds) {
    MotorsState ms = motors_current_state();
    GeometryContext ctx = {
        .lat_deg = mount_internal_state.lat,
        .lon_deg = mount_internal_state.lon,
        .unix_seconds = unix_seconds,
        .limits = {
            .ra_min = ms.limits.ra_min,
            .ra_max = ms.limits.ra_max,
            .dec_min = ms.limits.dec_min,
            .dec_max = ms.limits.dec_max,
        },
    };
    return ctx;
}

bool equatorial_to_axis(EquatorialCoordinates eq, AxisCoordinates current,
                        double unix_seconds, PierSideRequirement required,
                        float speed_dps, AxisCoordinates *out) {
    GeometryContext ctx = context_at(unix_seconds);
    return geometry_equatorial_to_axis(&ctx, eq, current, required, speed_dps, out);
}

EquatorialCoordinates axis_to_equatorial(AxisCoordinates axis) {
    /* Fractional-second clock — avoids the ~15"/s sawtooth in reported RA. */
    struct timeval tv;
    gettimeofday(&tv, NULL);
    double now = (double) tv.tv_sec + (double) tv.tv_usec / 1e6;

    GeometryContext ctx = context_at(now);
    return geometry_axis_to_equatorial(&ctx, axis);
}

/* =========================================================================
 * HMS / DMS formatting
 * ========================================================================= */

RaHMS ra_hours_to_hms(float ra_hours) {
    RaHMS out;
    float total = fabsf(ra_hours);
    out.hours   = (int)total;
    float rem   = (total - (float)out.hours) * 60.0f;
    out.minutes = (int)rem;
    out.seconds = (rem - (float)out.minutes) * 60.0f;
    return out;
}

DecDMS dec_deg_to_dms(float dec_deg) {
    DecDMS out;
    out.sign    = (dec_deg >= 0.0f) ? 1 : -1;
    float total = fabsf(dec_deg);
    out.degrees = (int)total;
    float rem   = (total - (float)out.degrees) * 60.0f;
    out.minutes = (int)rem;
    out.seconds = (rem - (float)out.minutes) * 60.0f;
    return out;
}
