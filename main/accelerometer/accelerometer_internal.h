#pragma once

#include "accelerometer.h"

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

/* ── I2C bus ──────────────────────────────────────────────── */

#define ACCEL_I2C_PORT      I2C_NUM_0
#define ACCEL_SDA_GPIO      2
#define ACCEL_SCL_GPIO      1
#define ACCEL_CLK_HZ        100000   /* 100 kHz standard mode */
#define ACCEL_TIMEOUT_MS    100

/* ── Device addresses (ADXL345 SDO strapping) ─────────────── */

#define ACCEL_ADDR_SDO_GND  0x53
#define ACCEL_ADDR_SDO_3V3  0x1D
#define ACCEL_SENSOR_COUNT  2

/* ── ADXL345 register map ─────────────────────────────────── */

#define ADXL345_REG_DEVID        0x00
#define ADXL345_REG_POWER_CTL    0x2D
#define ADXL345_REG_DATA_FORMAT  0x31
#define ADXL345_REG_DATAX0       0x32

#define ADXL345_DEVID_EXPECTED       0xE5
#define ADXL345_POWER_CTL_MEASURE    0x08   /* start measurement mode */

/* ── Scaling ──────────────────────────────────────────────── */

#define ADXL345_LSB_PER_G    256.0f   /* ±2 g, 10-bit: 3.9 mg/LSB */
#define ACCEL_DEG_PER_RAD    57.2957795131f

/* ── Read cadence ─────────────────────────────────────────── */

#define ACCEL_READ_PERIOD_US  500000   /* 500 ms */

/* ── Shared state ─────────────────────────────────────────── */

typedef struct {
    uint8_t                  address;
    i2c_master_dev_handle_t  dev_handle;
    bool                     present;
} AccelSensor;

/* One physical reading, already converted to engineering units. */
typedef struct {
    float x_g;
    float y_g;
    float z_g;
    float tilt_deg;      /* deviation from level, 0-90° */
    float heading_deg;   /* tilt direction in the X-Y plane, 0-360° */
} AccelSample;

extern AccelSensor accel_sensors[ACCEL_SENSOR_COUNT];

/* ── Internal helpers ─────────────────────────────────────── */

/* Read a single register into *value. */
esp_err_t accelerometer_read_register(const AccelSensor *sensor, uint8_t reg, uint8_t *value);

/* Write a single register. */
esp_err_t accelerometer_write_register(const AccelSensor *sensor, uint8_t reg, uint8_t value);

/* Read the 6 raw data bytes and convert them to g and tilt angles. */
esp_err_t accelerometer_read_sample(const AccelSensor *sensor, AccelSample *out);
