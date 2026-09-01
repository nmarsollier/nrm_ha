/* LED — led_breathe_cb.c
 *
 * Purpose: breathing animation for the ERROR state.
 *
 * Slow sinusoidal-like breathing:
 *   Timer fires every LED_BREATHE_SLOW_MS (1500 ms).
 *   Each tick toggles the fade direction: dim → bright → dim → …
 *   The 1500 ms hardware fade gives a smooth, ominous pulse.
 */

#include "led_internal.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "LED_BREATHE";

static esp_timer_handle_t s_breathe_timer;
static bool               s_breathe_up;   /* current fade direction */

static void breathe_tick(void *arg) {
    (void) arg;

    if (s_breathe_up) {
        ledc_set_fade_time_and_start(LED_MODE, LED_CHANNEL,
                                     LED_DIM_DUTY, LED_BREATHE_SLOW_MS,
                                     LEDC_FADE_NO_WAIT);
        s_breathe_up = false;
    } else {
        ledc_set_fade_time_and_start(LED_MODE, LED_CHANNEL,
                                     LED_BRIGHT_DUTY, LED_BREATHE_SLOW_MS,
                                     LEDC_FADE_NO_WAIT);
        s_breathe_up = true;
    }
}

void led_breathe_start(void) {
    if (s_breathe_timer) {
        /* Already running — nothing to do. */
        return;
    }

    s_breathe_up = true;

    /* Start immediately: fade to bright so the first visible effect is a rise. */
    ledc_set_fade_time_and_start(LED_MODE, LED_CHANNEL,
                                 LED_BRIGHT_DUTY, LED_BREATHE_SLOW_MS,
                                 LEDC_FADE_NO_WAIT);

    uint32_t period_us = LED_BREATHE_SLOW_MS * 1000UL;

    esp_timer_create_args_t args = {
        .callback = breathe_tick,
        .arg      = NULL,
        .name     = "led_breathe",
    };
    esp_timer_create(&args, &s_breathe_timer);
    esp_timer_start_periodic(s_breathe_timer, period_us);

    ESP_LOGI(TAG, "breathing started (period=%lu us)", (unsigned long) period_us);
}

void led_breathe_stop(void) {
    if (!s_breathe_timer) {
        return;
    }

    esp_timer_stop(s_breathe_timer);
    esp_timer_delete(s_breathe_timer);
    s_breathe_timer = NULL;

    ESP_LOGI(TAG, "stopped");
}
