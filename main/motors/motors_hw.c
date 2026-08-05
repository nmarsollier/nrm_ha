/* Motors - motors_hw.c
 *
 * Purpose: DIR, ENABLE, and ALARM GPIO control for the closed-loop
 * integrated stepper drivers (NRM-HA).
 *
 * STEP pulse generation is handled by the RMT peripheral (motors_rmt.c)
 * for jitter-free hardware-timed pulses with DMA streaming.
 *
 * Hardware: NEMA 17 closed-loop stepper motors with integrated drivers,
 * 64-microstep DIP-switch setting, 300:1 total reduction.
 */

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "motors_internal.h"

static const char *TAG = "MOTORS_HW";

/* Cached last directions to avoid redundant GPIO writes. */
static int last_dir_ra = -1;
static int last_dir_dec = -1;

esp_err_t motors_hw_init(void) {
    /*
     * DIR and ENABLE pins are managed via GPIO.
     * STEP pins (GPIO 13, GPIO 10) are owned by the RMT peripheral and
     * configured by motors_rmt_init().
     */
    const uint64_t pin_mask =
            (1ULL << RA_DIR_GPIO) |
            (1ULL << DEC_DIR_GPIO) |
            (1ULL << MOTORS_ENABLE_GPIO);

    gpio_config_t config = {
        .pin_bit_mask = pin_mask,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t result = gpio_config(&config);

    if (result != ESP_OK) {
        return result;
    }

    last_dir_ra = 0;
    last_dir_dec = 0;

    motors_hw_enable();
    motors_hw_alarm_init();

    return ESP_OK;
}

void motors_hw_enable(void) {
    gpio_set_level(MOTORS_ENABLE_GPIO, MOTORS_ENABLE_ACTIVE_LEVEL);
}

void motors_hw_disable(void) {
    gpio_set_level(MOTORS_ENABLE_GPIO, MOTORS_ENABLE_INACTIVE_LEVEL);
}

void motors_hw_set_direction_ra(MotorDirection direction) {
    int dir = direction == MOTOR_DIRECTION_POSITIVE ? 0 : 1;
    if (last_dir_ra != dir) {
        last_dir_ra = dir;
        gpio_set_level(RA_DIR_GPIO, dir);
    }
}

void motors_hw_set_direction_dec(MotorDirection direction) {
    int dir = direction == MOTOR_DIRECTION_POSITIVE ? 1 : 0;
    if (last_dir_dec != dir) {
        last_dir_dec = dir;
        gpio_set_level(DEC_DIR_GPIO, dir);
    }
}

/* ── ALARM monitoring ─────────────────────────────────────────── */

void motors_hw_alarm_init(void) {
    const uint64_t alarm_mask =
            (1ULL << RA_ALARM_GPIO) |
            (1ULL << DEC_ALARM_GPIO);

    gpio_config_t alarm_config = {
        .pin_bit_mask = alarm_mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t result = gpio_config(&alarm_config);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "ALARM GPIO config failed: %s", esp_err_to_name(result));
    }
}

/*
 * Return true if either closed-loop driver has asserted its ALARM output.
 * ALARM is active-low: LOW = fault condition (position error, stall, overcurrent).
 */
bool motors_hw_alarm_asserted(void) {
    return gpio_get_level(RA_ALARM_GPIO) == 0 ||
           gpio_get_level(DEC_ALARM_GPIO) == 0;
}
