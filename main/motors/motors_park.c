/* Motors - motors_park.c
 *
 * Purpose: park both axes immediately.
 *
 * Stops the motion loop, drains RMT, and publishes PARKED only after the motion
 * task has confirmed rest at the stop generation.
 */
#include "motors.h"
#include "motors_internal.h"

MotorResultCode motors_park(void) {
    if (!motors_control_lock()) {
        return MOTOR_ERR_HARDWARE_ERROR;
    }

    MotorResultCode rc = motors_stop_and_wait();
    if (rc != MOTOR_OK) {
        motors_control_unlock();
        return rc;
    }

    /* Publish PARKED and read the final status in one critical section, so a
     * fault that lands after the drain result is reported as an error, never as
     * a successful park.  Never overwrite a latched ERROR. */
    portENTER_CRITICAL(&motors_state_lock);
    if (motors_state.status != MOTORS_STATUS_ERROR) {
        motors_state.status = MOTORS_STATUS_PARKED;
    }
    motors_state.tracking = TRACKING_NONE;
    rc = motors_status_is_error(motors_state.status)
         ? MOTOR_ERR_HARDWARE_ERROR : MOTOR_OK;
    portEXIT_CRITICAL(&motors_state_lock);

    /* Reopen admission only when no ERROR is latched; a fault that landed after
     * the status check keeps admission closed and is reported. */
    if (!motors_admission_open()) {
        rc = MOTOR_ERR_HARDWARE_ERROR;
    }
    motors_control_unlock();
    return rc;
}
