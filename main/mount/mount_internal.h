#pragma once

#include "mount.h"

#include <stdbool.h>
#include <stdint.h>

#include "motors/motors.h"

/* Internal mount state stored by the mount module. */
extern MountSettings mount_internal_state;

MountResult mount_result_ok(void);

MountResult mount_result_error(const char *message);

/* NVS helpers used to persist mount settings. */
void mount_settings_load(MountSettings *out_settings);

void mount_settings_save(const MountSettings *settings);

MountResult motors_result_code_error_result(MotorResultCode rc);

/*
 * Check whether the mount is in the unrecoverable ERROR state (motor fault
 * or missing accelerometer).  All mount command functions gate on this
 * before delegating to motors.
 */
static inline bool mount_is_error(void) {
    return motors_status_is_error(motors_current_state().status);
}

static inline MountResult mount_result_error_state(void) {
    if (motors_current_state().status == MOTORS_STATUS_ACCEL_ERROR) {
        return mount_result_error("Accelerometer not found — mount in error state");
    }
    return mount_result_error("Motors in error state — reboot required");
}
