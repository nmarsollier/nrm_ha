#pragma once

#include "esp_err.h"

/*
 * ADXL345 accelerometer on the I2C bus (GPIO2 = SDA, GPIO1 = SCL).
 *
 * One sensor, with its SDO pin tied to GND (address 0x53).
 *
 * Every 500 ms the acceleration and tilt orientation of the sensor is
 * read and logged.  The sensor is a required peripheral: if it does not
 * answer the probe at boot, the mount enters the ERROR state.
 */

/*
 * Bring up the I2C bus, probe the sensor and configure it if present.
 * Returns ESP_OK when the sensor is present, or an error otherwise.
 */
esp_err_t accelerometer_init(void);

/*
 * Periodic update, call every ~100 ms from the runtime loop.
 *
 * Reads and logs acceleration and orientation of the sensor, throttled
 * to one read every 500 ms.
 */
void accelerometer_update(void);
