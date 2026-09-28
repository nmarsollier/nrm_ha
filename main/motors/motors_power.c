/* Motors - motors_power.c
 *
 * Purpose: gate motor motion on the external-power rail.  When the 12V
 * switch is off (USB-only) the motors enter ERROR and stay there until
 * power returns.
 */
#include "motors.h"
#include "motors_internal.h"

#include "esp_log.h"

static const char *TAG = "MOTORS_POWER";

/* True when the current ERROR was caused by a power loss (recoverable),
 * as opposed to a hardware fault (unrecoverable). */
static bool s_power_fault = false;

void motors_update_power(bool power_ok) {
    if (!power_ok) {
        if (!s_power_fault) {
            ESP_LOGW(TAG, "external power lost — motors in ERROR");
        }
        motors_enter_error_state();
        s_power_fault = true;
        return;
    }

    if (s_power_fault) {
        ESP_LOGI(TAG, "external power restored — motors back to READY");
        s_power_fault = false;
        motors_state.status = MOTORS_STATUS_READY;
        motors_state.tracking = TRACKING_NONE;
    }
}
