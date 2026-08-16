/* Accelerometer — accelerometer_update.c
 *
 * Purpose: periodic read-and-log of every present sensor.
 *
 * Called from the runtime loop every ~100 ms.  Uses esp_timer_get_time()
 * to throttle actual reads to one every ACCEL_READ_PERIOD_US (500 ms),
 * so the cadence does not depend on the loop period.
 */
#include "accelerometer_internal.h"

#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "ACCELEROMETER_UPDATE";

void accelerometer_update(void) {
    static int64_t last_read_us = 0;

    int64_t now_us = esp_timer_get_time();
    if (now_us - last_read_us < ACCEL_READ_PERIOD_US) {
        return;
    }
    last_read_us = now_us;

    for (int i = 0; i < ACCEL_SENSOR_COUNT; i++) {
        const AccelSensor *sensor = &accel_sensors[i];
        if (!sensor->present) {
            continue;
        }

        AccelSample sample;
        esp_err_t err = accelerometer_read_sample(sensor, &sample);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "addr 0x%02X: read failed: %s",
                     sensor->address, esp_err_to_name(err));
            continue;
        }

        ESP_LOGI(TAG,
                 "addr 0x%02X: x=%.3fg y=%.3fg z=%.3fg | tilt=%.1f deg heading=%.1f deg",
                 sensor->address,
                 sample.x_g, sample.y_g, sample.z_g,
                 sample.tilt_deg, sample.heading_deg);
    }
}
