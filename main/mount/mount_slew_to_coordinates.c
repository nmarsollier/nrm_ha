/* Mount - mount_slew_to_coordinates.c
 *
 * Purpose: move the mount to a requested equatorial position.
 */
#include "mount.h"
#include "mount_internal.h"

#include "motors.h"
#include "mount_profile.h"

MountResult mount_slew_to_coordinates(float ra, float dec, int speed_rate,
                                     PierSideRequirement required) {
    if (mount_is_error()) {
        return mount_result_error_state();
    }

    /* GOTO needs a valid clock: without it LST/HA are wrong and the mount
     * would point at the wrong sky position. */
    if (!mount_time_valid) {
        return mount_result_error("Time not synchronized — set the UTC date first");
    }

    /* An explicit meridian flip (side request) does not apply to a fork. */
    if (mount_profile.geometry == GEOMETRY_FORK && required != PIER_SIDE_AUTO) {
        return mount_result_error("Meridian flip not supported on this mount");
    }

    EquatorialCoordinates eq = { .ra_hours = ra, .dec_deg = dec };
    AxisCoordinates current = { .ra_axis_deg = motors_get_ra_deg(),
                                .dec_axis_deg = motors_get_dec_deg() };
    float speed = motors_get_slewing_speed(speed_rate);

    /* A single geometry context: one snapshot of site, limits and instant.  The
     * geometry derives the arrival instant internally (same site/limits). */
    MotorsState ms = motors_current_state();
    GeometryContext ctx = {
        .lat_deg = mount_internal_state.lat,
        .lon_deg = mount_internal_state.lon,
        .unix_seconds = mount_unix_now(),
        .limits = { ms.limits.ra_min, ms.limits.ra_max,
                    ms.limits.dec_min, ms.limits.dec_max },
    };

    GotoPlan plan;
    if (!geometry_plan_goto(&ctx, eq, current, required, speed, &plan)) {
        return mount_result_error("Target unreachable — no valid axis solution");
    }

    /* Geometric horizon (fork only): reject a goto whose monotonic RA/DEC
     * trajectory dips below the horizon, even when the mechanical limits admit
     * it. */
    if (mount_profile.geometry == GEOMETRY_FORK
        && !geometry_path_above_horizon(current.ra_axis_deg, current.dec_axis_deg,
                                        plan.target.ra_axis_deg, plan.target.dec_axis_deg,
                                        ctx.lat_deg)) {
        return mount_result_error("Target below horizon — path crosses the horizon");
    }

    MotorResultCode rc1 = motors_slew_to_angle(plan.target.ra_axis_deg,
                                               plan.target.dec_axis_deg, speed_rate);
    if (rc1 != MOTOR_OK) {
        /* Propagate the specific code (queue full, parked, out of range) so a
         * rejected goto never reads as a generic success and clients get the
         * same "command queue full — try again" contract as PulseGuide. */
        return motors_result_code_error_result(rc1);
    }

    return mount_result_ok();
}
