#pragma once

/*
 * ADXL345 accelerometer on the I2C bus (GPIO2 = SDA, GPIO1 = SCL).
 *
 * Supports up to two sensors on the same bus, selected by the SDO pin:
 *
 *   SDO -> GND   = 0x53
 *   SDO -> 3V3   = 0x1D
 *
 * Every 500 ms the acceleration and tilt orientation of each sensor that
 * answers is read and logged.  Sensors that are not physically present
 * are ignored — the mount works fine without them, no error is raised.
 */

/* Bring up the I2C bus, probe both addresses and configure the sensors found. */
void accelerometer_init(void);

/*
 * Periodic update, call every ~100 ms from the runtime loop.
 *
 * Reads and logs acceleration and orientation of every present sensor,
 * throttled to one read every 500 ms.
 */
void accelerometer_update(void);
