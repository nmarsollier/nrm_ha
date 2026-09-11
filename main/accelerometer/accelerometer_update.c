/* Accelerometer — accelerometer_update.c
 *
 * Purpose: periodic read-and-log of every present sensor.
 *
 * Called from the runtime loop every ~100 ms.  Uses esp_timer_get_time()
 * to throttle reads with an adaptive cadence: fast during a slew, slow
 * otherwise.
 */
#include "accelerometer_internal.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "motors/motors.h"

static const char *TAG = "ACCELEROMETER_UPDATE";

void accelerometer_update(void) {
    if (accelerometer_is_calibrating()) {
        return;  /* the calibration task owns the sensor */
    }

    static int64_t last_read_us = 0;

    int64_t now_us = esp_timer_get_time();
    int64_t period_us = (motors_current_state().status == MOTORS_STATUS_SLEWING)
                            ? ACCEL_READ_PERIOD_SLEW_US
                            : ACCEL_READ_PERIOD_US;
    if (now_us - last_read_us < period_us) {
        return;
    }
    last_read_us = now_us;

    const AccelSensor *sensor = &accel_sensor;
    if (!sensor->present) {
        return;
    }

    AccelSample sample;
    esp_err_t err = accelerometer_read_sample(sensor, &sample);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "addr 0x%02X: read failed: %s",
                 sensor->address, esp_err_to_name(err));
        return;
    }

    accelerometer_sample_store(&sample);
}
