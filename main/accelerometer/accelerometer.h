#pragma once

#include <stdbool.h>
#include "esp_err.h"

/*
 * ADXL345 accelerometer on the I2C bus (GPIO4 = SDA, GPIO5 = SCL).
 *
 * One sensor, with its SDO pin tied to GND (address 0x53).
 *
 * Every 500 ms the acceleration and tilt orientation of the sensor is
 * read and logged.  The sensor is a required peripheral: if it does not
 * answer the probe at boot, the mount enters the ERROR state.
 */

/*
 * One physical reading, already converted to engineering units.
 *
 * x_g/y_g/z_g are acceleration in g.  tilt_deg is the angle of the sensor
 * Z axis from vertical (0-90°).  heading_deg is the tilt direction in the
 * X-Y plane (0-360°).
 */
typedef struct {
    float x_g;
    float y_g;
    float z_g;
    float tilt_deg;
    float heading_deg;
} AccelSample;

/*
 * Bring up the I2C bus, probe the sensor and configure it if present.
 * Returns ESP_OK when the sensor is present, or an error otherwise.
 */
esp_err_t accelerometer_init(void);

/*
 * True when the ADXL345 was probed and configured at boot.  Exposed for
 * the UI to report the accelerometer status separately from the motors.
 */
bool accelerometer_is_present(void);

/*
 * Periodic update, call every ~100 ms from the runtime loop.
 * Reads and logs the sensor, throttled to one read every 500 ms.
 */
void accelerometer_update(void);

/*
 * Copy the latest reading into *out.  Returns false until the first
 * successful read.
 */
bool accelerometer_get_sample(AccelSample *out);

/*
 * Polar-axis elevation above the horizon in degrees (0-90°), equal to the
 * site latitude when the mount is polar-aligned.  Returns a negative value
 * until the polar axis has been calibrated (see accelerometer_calibrate_start).
 */
float accelerometer_get_elevation_deg(void);

/*
 * Start an automatic polar-axis calibration: the mount slews RA by itself
 * through known angles, captures gravity at each, then fits and persists
 * the polar axis.  Keep the mount clear of obstacles while it runs.
 */
void accelerometer_calibrate_start(void);

/* True while an automatic calibration is in progress. */
bool accelerometer_is_calibrating(void);

/*
 * RA rotation angle measured by the accelerometer (0-360°), independent of
 * the motor step counters.  Negative when unavailable (uncalibrated or no
 * reading yet).
 */
float accelerometer_get_ra_angle_deg(void);

/*
 * Accelerometer RA angle relative to home (-180..180°, 0° at home), for
 * direct comparison with the motor step counter.  Returns a large negative
 * value when unavailable.
 */
float accelerometer_get_ra_signed_deg(void);

/* Record the current accelerometer RA angle as the home (0°) reference. */
void accelerometer_set_ra_home(void);

/*
 * True when the accelerometer RA angle is outside the motor limits (plus a
 * small margin).  Used by the motion task as a backup limit check.
 */
bool accelerometer_ra_limit_exceeded(void);
