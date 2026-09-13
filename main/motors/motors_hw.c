/* Motors - motors_hw.c
 *
 * Purpose: DIR GPIO control and TMC2209 initialisation for the two
 * stepper axes (NRM-HA).
 *
 * STEP pulse generation is handled by the RMT peripheral (motors_rmt.c)
 * for jitter-free hardware-timed pulses with DMA streaming.
 * ENABLE is hardwired (always enabled) — no GPIO control needed.
 *
 * Hardware: NEMA 17 stepper motors driven by TMC2209 in STEP/DIR mode
 * with UART-configured microstepping.  Direct 3.3 V logic — no level
 * shifting.  300:1 total reduction.
 */

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "motors_internal.h"
#include "tmc/tmc.h"

/* Cached last directions to avoid redundant GPIO writes. */
static int last_dir_ra = -1;
static int last_dir_dec = -1;

esp_err_t motors_hw_init(void) {
    /*
     * DIR pins are managed via GPIO.
     * STEP pins (GPIO 14, GPIO 11) are owned by the RMT peripheral and
     * configured by motors_rmt_init().
     */
    const uint64_t pin_mask =
            (1ULL << RA_DIR_GPIO) |
            (1ULL << DEC_DIR_GPIO);

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

    /* Configure and verify both TMC2209 drivers over UART. */
    esp_err_t tmc_result = tmc2209_hw_init();
    if (tmc_result != ESP_OK) {
        return tmc_result;
    }

    return ESP_OK;
}

void motors_hw_set_direction_ra(MotorDirection direction) {
    int dir = direction == MOTOR_DIRECTION_POSITIVE ? 1 : 0;
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
