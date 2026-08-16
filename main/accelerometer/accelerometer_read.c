/* Accelerometer — accelerometer_read.c
 *
 * Purpose: read raw acceleration from an ADXL345 and convert it to
 * engineering units.
 *
 * The 6 data bytes at DATAX0..DATAZ1 hold X, Y, Z as little-endian,
 * two's complement 16-bit values.  Acceleration is scaled by 256 LSB/g
 * (±2 g range).  Tilt and heading come from the direction of the gravity
 * vector, so they do not depend on the scale factor.
 */
#include "accelerometer_internal.h"

#include <math.h>

esp_err_t accelerometer_read_sample(const AccelSensor *sensor, AccelSample *out) {
    uint8_t reg = ADXL345_REG_DATAX0;
    uint8_t data[6] = { 0 };

    esp_err_t err = i2c_master_transmit_receive(sensor->dev_handle, &reg, 1, data, 6, ACCEL_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }

    int16_t raw_x = (int16_t)((data[1] << 8) | data[0]);
    int16_t raw_y = (int16_t)((data[3] << 8) | data[2]);
    int16_t raw_z = (int16_t)((data[5] << 8) | data[4]);

    float x = raw_x / ADXL345_LSB_PER_G;
    float y = raw_y / ADXL345_LSB_PER_G;
    float z = raw_z / ADXL345_LSB_PER_G;

    /* Deviation from level: angle between the Z axis and vertical, 0-90°. */
    float magnitude = sqrtf(x * x + y * y + z * z);
    float tilt = 0.0f;
    if (magnitude > 0.01f) {
        tilt = acosf(fabsf(z) / magnitude) * ACCEL_DEG_PER_RAD;
    }

    /* Direction of the tilt in the X-Y plane: 0° along +X, 90° along +Y. */
    float heading = atan2f(y, x) * ACCEL_DEG_PER_RAD;
    if (heading < 0.0f) {
        heading += 360.0f;
    }

    out->x_g = x;
    out->y_g = y;
    out->z_g = z;
    out->tilt_deg = tilt;
    out->heading_deg = heading;

    return ESP_OK;
}
