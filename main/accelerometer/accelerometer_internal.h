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

/* ── Device address (ADXL345, SDO tied to GND) ────────────── */

#define ACCEL_ADDR_SDO_GND  0x53 // 0x1D o 0x53

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

#define ACCEL_READ_PERIOD_US       500000   /* 500 ms — idle / tracking */
#define ACCEL_READ_PERIOD_SLEW_US  100000   /* 100 ms — slewing, limit responsiveness */

/* ── Validity ─────────────────────────────────────────────── */

#define ACCEL_G_TOLERANCE  0.25f   /* |g| - 1 tolerance — loose, to accept zero-g offset */

/* ── Shared state ─────────────────────────────────────────── */

typedef struct {
    uint8_t                  address;
    i2c_master_dev_handle_t  dev_handle;
    bool                     present;
} AccelSensor;

extern AccelSensor accel_sensor;

/* Store the latest validated reading for accelerometer_get_sample(). */
void accelerometer_sample_store(const AccelSample *sample);

/* Load the persisted polar axis (called once from accelerometer_init()). */
void accelerometer_calibrate_load(void);

/* ── Internal helpers ─────────────────────────────────────── */

/* Read a single register into *value. */
esp_err_t accelerometer_read_register(const AccelSensor *sensor, uint8_t reg, uint8_t *value);

/* Write a single register. */
esp_err_t accelerometer_write_register(const AccelSensor *sensor, uint8_t reg, uint8_t value);

/* Read the 6 raw data bytes and convert them to g and tilt angles. */
esp_err_t accelerometer_read_sample(const AccelSensor *sensor, AccelSample *out);
