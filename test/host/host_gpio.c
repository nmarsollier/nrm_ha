/* Host — GPIO mock: records the DIR levels written by motors_hw_*. */
#include <pthread.h>

#include "driver/gpio.h"
#include "host_internal.h"
#include "mount_profile.h"

int host_dir_level[2] = {0, 0};
static pthread_mutex_t s_dir_mtx = PTHREAD_MUTEX_INITIALIZER;

esp_err_t gpio_config(const gpio_config_t *cfg) {
    (void) cfg;
    return ESP_OK;
}

esp_err_t gpio_set_level(gpio_num_t gpio, uint32_t level) {
    /* Track the two DIR pins so the RMT mock can attribute a STEP's direction. */
    pthread_mutex_lock(&s_dir_mtx);
    if (gpio == mount_profile.hardware.axes[AXIS_RA].dir_gpio) {
        host_dir_level[0] = level ? 1 : 0;
    } else if (gpio == mount_profile.hardware.axes[AXIS_DEC].dir_gpio) {
        host_dir_level[1] = level ? 1 : 0;
    }
    pthread_mutex_unlock(&s_dir_mtx);
    return ESP_OK;
}

/* Current DIR GPIO level for an axis (0/1), read under the DIR mutex. */
int host_dir_level_read(int axis) {
    if (axis != 0 && axis != 1) return -1;
    pthread_mutex_lock(&s_dir_mtx);
    int v = host_dir_level[axis];
    pthread_mutex_unlock(&s_dir_mtx);
    return v;
}

/* Test-only: force the DIR level for an axis, so a harness self-test can change
 * DIR between accept and emit (a unit test of the simulator, not of production).
 * Invalidate the production DIR-write cache so the next direction set always
 * writes the GPIO and re-synchronises the physical level. */
void host_dir_level_set(int axis, int level) {
    if (axis != 0 && axis != 1) return;
    pthread_mutex_lock(&s_dir_mtx);
    host_dir_level[axis] = level ? 1 : 0;
    pthread_mutex_unlock(&s_dir_mtx);
    motors_hw_reset_dir_cache();
}
