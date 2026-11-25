/* Mount - mount_destination_side_of_pier.c
 *
 * Purpose: predict, without moving, which pier side a goto to the given
 * equatorial position would select at this instant.  Backs the ASCOM
 * DestinationSideOfPier property; it must select exactly as a real goto would,
 * so it uses the same geometry_plan_goto() planner as mount_slew_to_coordinates.
 */
#include "mount.h"
#include "mount_internal.h"

#include "motors.h"
#include "mount_profile.h"

MountResult mount_destination_side_of_pier(float ra, float dec, int *side) {
    if (!mount_time_valid) {
        return mount_result_error("Time not synchronized — set the UTC date first");
    }

    /* The fork has no pier side — this query does not apply to it. */
    if (mount_profile.geometry == GEOMETRY_FORK) {
        return mount_result_error("Pier side not applicable to this mount");
    }

    EquatorialCoordinates eq = { .ra_hours = ra, .dec_deg = dec };
    AxisCoordinates current = { .ra_axis_deg = motors_get_ra_deg(),
                                .dec_axis_deg = motors_get_dec_deg() };
    float speed = motors_get_slewing_speed(0);   /* default goto speed */

    MotorsState ms = motors_current_state();
    GeometryContext ctx = {
        .lat_deg = mount_internal_state.lat,
        .lon_deg = mount_internal_state.lon,
        .unix_seconds = mount_unix_now(),
        .limits = { ms.limits.ra_min, ms.limits.ra_max,
                    ms.limits.dec_min, ms.limits.dec_max },
    };

    GotoPlan plan;
    if (!geometry_plan_goto(&ctx, eq, current, PIER_SIDE_AUTO, speed, &plan)) {
        return mount_result_error("Target unreachable — no valid axis solution");
    }

    if (side != NULL) {
        *side = plan.pier_side;
    }
    return mount_result_ok();
}
