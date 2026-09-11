/* Motors - motors_enter_accel_error_state.c
 *
 * Purpose: put the motors subsystem into the accelerometer-not-found
 * error state.
 *
 * Called at boot when the ADXL345 (required peripheral) fails its I2C
 * probe.  Unlike motors_enter_error_state(), no RMT abort or guiding
 * clear is needed: there is no motion yet at boot and the motors
 * hardware is fine.  Only a reboot can clear this state.
 */
#include "motors_internal.h"

void motors_enter_accel_error_state(void) {
    motors_state.status = MOTORS_STATUS_ACCEL_ERROR;
}
