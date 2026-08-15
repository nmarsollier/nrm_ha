/* Buzzer — buzzer_init.c
 *
 * Purpose: initialise LEDC PWM on GPIO 5 for the passive event buzzer.
 *
 * Configures timer 1 at 2 kHz, 13-bit resolution (8192 steps), binds
 * channel 1 to GPIO 5, starts silent (duty 0), and emits the power-on
 * beep.
 *
 * GPIO 5 drives a UMC2003 Darlington channel (open-collector sink):
 * GPIO HIGH → output sinks → buzzer active.  No output inversion.
 */
#include "buzzer_internal.h"

#include "esp_err.h"
#include "esp_log.h"

static const char *TAG = "BUZZER_INIT";

void buzzer_init(void) {
    ledc_timer_config_t timer_conf = {
        .speed_mode      = BUZZER_MODE,
        .duty_resolution = BUZZER_DUTY_RES,
        .timer_num       = BUZZER_TIMER,
        .freq_hz         = BUZZER_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
        .deconfigure     = false,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer_conf));

    ledc_channel_config_t chan_conf = {
        .gpio_num       = BUZZER_GPIO,
        .speed_mode     = BUZZER_MODE,
        .channel        = BUZZER_CHANNEL,
        .intr_type      = LEDC_INTR_DISABLE,
        .timer_sel      = BUZZER_TIMER,
        .duty           = BUZZER_OFF_DUTY,
        .hpoint         = 0,
        .sleep_mode     = LEDC_SLEEP_MODE_NO_ALIVE_NO_PD,
        .flags          = { .output_invert = 0 },
    };
    ESP_ERROR_CHECK(ledc_channel_config(&chan_conf));

    ESP_LOGI(TAG, "LEDC PWM ready on GPIO %d, %d Hz", BUZZER_GPIO, BUZZER_FREQ_HZ);

    buzzer_play(BUZZER_PATTERN_BOOT);
}
