#include "mount.h"
#include "mount_internal.h"
#include "motors.h"

MountResult mount_set_move_axis_speed(float ra_speed, float dec_speed) {
    if (mount_is_error()) {
        return mount_result_error_state();
    }

    MotorResultCode rc = motors_set_move_axis_speed(ra_speed, dec_speed);
    return motors_result_code_error_result(rc);
}

/*
 * Move a single axis (0 = RA, 1 = DEC) continuously at `rate` deg/s,
 * preserving the other axis's current rate (clamped by the motors module).
 * The read-modify-write of the other axis's rate is atomic, so concurrent
 * single-axis updates cannot lose each other's rate.
 */
MountResult mount_set_move_axis_rate(int axis, float rate) {
    return motors_result_code_error_result(motors_set_move_axis_rate(axis, rate));
}
