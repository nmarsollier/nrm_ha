#include "mount.h"
#include "mount_internal.h"
#include "motors.h"

#include <math.h>

static TrackingMode s_saved_tracking = TRACKING_NONE;

/* Per-axis MoveAxis rate cache (deg/s).  Lives in the mount layer so any
 * STOP — which calls mount_move_axis_reset() — clears it, preventing a stale
 * residual rate from a previous manual move leaking into the next MoveAxis. */
static float s_ra_rate = 0.0f;
static float s_dec_rate = 0.0f;

MountResult mount_set_move_axis_speed(float ra_speed, float dec_speed) {
    if (mount_is_error()) {
        return mount_result_error_state();
    }

    if (ra_speed == 0.0f && dec_speed == 0.0f) {
        /* Read before mount_stop() — mount_move_axis_reset() clears it. */
        TrackingMode to_restore = s_saved_tracking;
        s_saved_tracking = TRACKING_NONE;
        MountResult r = mount_stop();
        if (to_restore != TRACKING_NONE) {
            mount_set_tracking(to_restore);
        }
        return r;
    }

    MotorsState s = motors_current_state();
    if (s.status == MOTORS_STATUS_TRACKING && s.tracking != TRACKING_NONE) {
        /* Stop tracking first.  mount_stop() calls mount_move_axis_reset(),
         * which wipes both the saved tracking mode and the MoveAxis rate
         * cache — remember the tracking mode so a later full stop restores it. */
        TrackingMode saved = s.tracking;
        mount_stop();
        s_saved_tracking = saved;
    }

    /* Keep the per-axis rate cache in sync on every non-stop command so a later
     * single-axis change (mount_set_move_axis_rate) preserves the untouched
     * axis.  Skipping this made "stop one axis" stop both axes. */
    s_ra_rate = ra_speed;
    s_dec_rate = dec_speed;

    MotorResultCode rc = motors_set_move_axis_speed(ra_speed, dec_speed);
    return motors_result_code_error_result(rc);
}

/*
 * Move a single axis (0 = RA, 1 = DEC) continuously at `rate` deg/s,
 * preserving the other axis's current rate.  Clamps to the safe slew ceiling.
 */
MountResult mount_set_move_axis_rate(int axis, float rate) {
    float hi = motors_get_slewing_speed(4);
    float clamped = fmaxf(fminf(rate, hi), -hi);

    if (axis == 0) {
        s_ra_rate = clamped;
    } else if (axis == 1) {
        s_dec_rate = clamped;
    } else {
        return mount_result_error("Axis not supported");
    }

    return mount_set_move_axis_speed(s_ra_rate, s_dec_rate);
}

void mount_move_axis_reset(void) {
    s_saved_tracking = TRACKING_NONE;
    s_ra_rate = 0.0f;
    s_dec_rate = 0.0f;
}
