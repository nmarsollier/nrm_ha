/* Host — GPIO mock: records the DIR levels written by motors_hw_*. */
#include "driver/gpio.h"
#include "host_internal.h"
#include "config.h"

int host_dir_level[2] = {0, 0};

esp_err_t gpio_config(const gpio_config_t *cfg) {
    (void) cfg;
    return ESP_OK;
}

esp_err_t gpio_set_level(gpio_num_t gpio, uint32_t level) {
    /* Track the two DIR pins so the RMT mock can attribute a STEP's direction. */
    if (gpio == RA_DIR_GPIO) {
        host_dir_level[0] = level ? 1 : 0;
    } else if (gpio == DEC_DIR_GPIO) {
        host_dir_level[1] = level ? 1 : 0;
    }
    return ESP_OK;
}
