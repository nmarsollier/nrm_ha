/* Host — monotonic clock + delay + error-name stubs. */
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include "esp_err.h"
#include "esp_rom_sys.h"

int64_t esp_timer_get_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t) ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

void esp_rom_delay_us(uint32_t us) {
    usleep(us);
}

const char *esp_err_to_name(esp_err_t err) {
    switch (err) {
        case ESP_OK:                 return "ESP_OK";
        case ESP_FAIL:               return "ESP_FAIL";
        case ESP_ERR_NO_MEM:         return "ESP_ERR_NO_MEM";
        case ESP_ERR_INVALID_ARG:    return "ESP_ERR_INVALID_ARG";
        case ESP_ERR_INVALID_STATE:  return "ESP_ERR_INVALID_STATE";
        case ESP_ERR_TIMEOUT:        return "ESP_ERR_TIMEOUT";
        default:                     return "ESP_ERR_OTHER";
    }
}
