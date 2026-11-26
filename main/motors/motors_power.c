/* Motors - motors_power.c
 *
 * Purpose: gate motor motion on the TMC2209 drivers being alive, sensed through
 * the UART probe (the drivers are fed by VM, so they are silent when the 12 V
 * rail is absent).  Both drivers must answer for the mount to move.  When power
 * is lost the motors enter ERROR and stay there until power returns, at which
 * point the TMC2209s are re-initialised (their registers were lost while
 * unpowered).
 */
#include "motors.h"
#include "motors_internal.h"
#include "tmc.h"

#include "esp_log.h"

static const char *TAG = "MOTORS_POWER";

/* Test hook — weak no-op overridden by the host harness to pause the recovery
 * after a successful TMC re-init but before READY is published. */
void __attribute__((weak)) motors_test_hook_after_power_reinit(void) {}

/* Cached power state, from the last TMC probe. */
static bool s_power_ok = true;

/* True when the current ERROR was caused by a power loss (recoverable),
 * as opposed to a hardware fault (unrecoverable). */
static bool s_power_fault = false;

void motors_mark_power_fault(void) {
    s_power_fault = true;
}

bool motors_has_power(void) {
    return s_power_ok;
}

void motors_update_power(bool power_ok) {
    s_power_ok = power_ok;

    if (!power_ok) {
        if (!s_power_fault) {
            ESP_LOGW(TAG, "TMC driver power lost — motors in ERROR");
        }
        motors_enter_error_state();
        s_power_fault = true;
        return;
    }

    if (!s_power_fault) {
        return;  /* power was already on — nothing to do */
    }

    /* Power returned — re-initialise the TMC2209s, whose registers were lost
     * while the drivers were unpowered. */
    s_power_fault = false;

    if (motors_has_hardware_fault()) {
        ESP_LOGI(TAG, "TMC power restored — hardware fault persists");
        return;
    }

    /* The power loss set ERROR and closed admission, but did not itself abort the
     * running motion: the executor aborts on its next loop iteration.  A fast
     * power return must not re-initialise the drivers (or reopen admission) while
     * RMT transmissions may still be in flight — wait for the executor to confirm
     * rest first, reusing the existing rest barrier. */
    if (!motors_motion_wait_idle(motors_stop_generation_get(), NULL)) {
        ESP_LOGE(TAG, "TMC power restored but executor did not confirm rest");
        motors_enter_hardware_fault();
        return;
    }

    if (tmc2209_reinit(mount_profile.axes[AXIS_RA].canonical_microsteps) == ESP_OK) {
        ESP_LOGI(TAG, "TMC power restored — motors back to READY");
        /* tmc2209_reinit() re-configured both drivers at the tracking
         * resolution and recorded it, so the microstep state is already in
         * sync — no separate reset needed. */
        motors_test_hook_after_power_reinit();
        /* Serialize the final READY + reopen transition with STOP/PARK: once
         * READY is published a STOP could legitimately begin, and this recovery
         * must not then reopen admission behind it.  The control mutex linearises
         * the two; motors_recover_to_ready() publishes READY and reopens
         * admission atomically (state→admission), only when no fault is latched. */
        if (!motors_control_lock()) {
            motors_enter_hardware_fault();
            return;
        }
        motors_recover_to_ready();
        motors_control_unlock();
    } else {
        ESP_LOGE(TAG, "TMC power restored but re-init failed — hardware fault");
        motors_enter_hardware_fault();
    }
}
