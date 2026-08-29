/* Accelerometer — accelerometer_init.c
 *
 * Purpose: bring up the I2C bus and the ADXL345 sensors on it.
 *
 * Creates the bus on GPIO2 (SDA) / GPIO1 (SCL), adds a device handle
 * for each of the two possible addresses (0x53 and 0x1D), probes them and
 * configures the ones that answer.  Missing sensors are logged and left
 * disabled — the mount keeps working without them.
 */
#include "accelerometer_internal.h"

#include "esp_log.h"

static const char *TAG = "ACCELEROMETER_INIT";

AccelSensor accel_sensors[ACCEL_SENSOR_COUNT];

esp_err_t accelerometer_read_register(const AccelSensor *sensor, uint8_t reg, uint8_t *value) {
    return i2c_master_transmit_receive(sensor->dev_handle, &reg, 1, value, 1, ACCEL_TIMEOUT_MS);
}

esp_err_t accelerometer_write_register(const AccelSensor *sensor, uint8_t reg, uint8_t value) {
    uint8_t buf[2] = { reg, value };
    return i2c_master_transmit(sensor->dev_handle, buf, sizeof(buf), ACCEL_TIMEOUT_MS);
}

/* Verify the device identity and switch it to measurement mode. */
static void configure_sensor(AccelSensor *sensor) {
    uint8_t devid = 0;
    if (accelerometer_read_register(sensor, ADXL345_REG_DEVID, &devid) != ESP_OK ||
        devid != ADXL345_DEVID_EXPECTED) {
        ESP_LOGW(TAG, "addr 0x%02X: unexpected DEVID 0x%02X (expected 0x%02X) — ignored",
                 sensor->address, devid, ADXL345_DEVID_EXPECTED);
        sensor->present = false;
        return;
    }

    /* ±2 g, 10-bit is the register default — write it explicitly for clarity. */
    accelerometer_write_register(sensor, ADXL345_REG_DATA_FORMAT, 0x00);
    accelerometer_write_register(sensor, ADXL345_REG_POWER_CTL, ADXL345_POWER_CTL_MEASURE);

    ESP_LOGI(TAG, "ADXL345 at 0x%02X ready", sensor->address);
}

void accelerometer_init(void) {
    const uint8_t addresses[ACCEL_SENSOR_COUNT] = {
        ACCEL_ADDR_SDO_GND,
        ACCEL_ADDR_SDO_3V3,
    };

    for (int i = 0; i < ACCEL_SENSOR_COUNT; i++) {
        accel_sensors[i].address    = addresses[i];
        accel_sensors[i].dev_handle = NULL;
        accel_sensors[i].present    = false;
    }

    i2c_master_bus_config_t bus_config = {
        .i2c_port               = ACCEL_I2C_PORT,
        .sda_io_num             = ACCEL_SDA_GPIO,
        .scl_io_num             = ACCEL_SCL_GPIO,
        .clk_source             = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt      = 7,
        .flags.enable_internal_pullup = true,
    };

    i2c_master_bus_handle_t bus_handle = NULL;
    esp_err_t err = i2c_new_master_bus(&bus_config, &bus_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "I2C bus init failed: %s — accelerometer disabled",
                 esp_err_to_name(err));
        return;
    }

    for (int i = 0; i < ACCEL_SENSOR_COUNT; i++) {
        i2c_device_config_t dev_config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address  = accel_sensors[i].address,
            .scl_speed_hz    = ACCEL_CLK_HZ,
        };

        err = i2c_master_bus_add_device(bus_handle, &dev_config,
                                        &accel_sensors[i].dev_handle);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "addr 0x%02X: device handle failed: %s",
                     accel_sensors[i].address, esp_err_to_name(err));
            continue;
        }

        /* Probe sends the address and checks for ACK. */
        err = i2c_master_probe(bus_handle, accel_sensors[i].address, ACCEL_TIMEOUT_MS);
        if (err != ESP_OK) {
            ESP_LOGI(TAG, "addr 0x%02X: not present (%s) — ignored",
                     accel_sensors[i].address, esp_err_to_name(err));
            continue;
        }

        accel_sensors[i].present = true;
        configure_sensor(&accel_sensors[i]);
    }
}
