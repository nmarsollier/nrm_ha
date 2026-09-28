/* Power - power.c
 *
 * Purpose: sense whether the mount is powered from the 12V rail (via the
 * LM2596 5.5V output) or is running on USB only.  Used to block motor
 * motion when the motors have no power.
 */
#include "power.h"
#include "config.h"

#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

static const char *TAG = "POWER";

/* 10k/10k divider on the 5.5V rail -> ~2.75V present, ~0V absent.
 * Raw threshold ~2000 (~1.5V) cleanly separates the two. */
#define POWER_PRESENT_THRESHOLD_RAW 2000

static adc_oneshot_unit_handle_t s_adc_handle;

#ifdef NRM_TEST_MODE
/* Test/bench override — compiled only under NRM_TEST_MODE. */
static bool s_debug_force_power = false;
static bool s_debug_force_value = false;

void power_debug_force(bool force, bool value) {
    s_debug_force_power = force;
    s_debug_force_value = value;
}
#endif

esp_err_t power_init(void) {
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = POWER_SENSE_ADC_UNIT,
    };

    esp_err_t err = adc_oneshot_new_unit(&unit_cfg, &s_adc_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_new_unit: %s", esp_err_to_name(err));
        return err;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_oneshot_config_channel(s_adc_handle, POWER_SENSE_ADC_CHANNEL, &chan_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_config_channel: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "external-power sensor ready (GPIO 1)");
    return ESP_OK;
}

bool power_has_external(void) {
#ifdef NRM_TEST_MODE
    if (s_debug_force_power) {
        return s_debug_force_value;
    }
#endif
    if (s_adc_handle == NULL) {
        /* Sensor not initialised — fail safe (block motion). */
        return false;
    }

    int raw = 0;
    if (adc_oneshot_read(s_adc_handle, POWER_SENSE_ADC_CHANNEL, &raw) != ESP_OK) {
        /* Read failure — fail safe (block motion). */
        return false;
    }
    return raw > POWER_PRESENT_THRESHOLD_RAW;
}
