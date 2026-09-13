/* Accelerometer — accelerometer_init.c
 *
 * Purpose: bring up the I2C bus and the ADXL345 sensor on it.
 *
 * Creates the bus on GPIO4 (SDA) / GPIO5 (SCL), adds a device handle
 * for the sensor address (0x53), probes it and configures it if it
 * answers.  A missing sensor is a fatal error: the mount starts in the
 * ERROR state.
 */
#include "accelerometer_internal.h"

#include "esp_log.h"

static const char *TAG = "ACCELEROMETER_INIT";

AccelSensor accel_sensor;

bool accelerometer_is_present(void) {
    return accel_sensor.present;
}

esp_err_t accelerometer_read_register(const AccelSensor *sensor, uint8_t reg, uint8_t *value) {
    return i2c_master_transmit_receive(sensor->dev_handle, &reg, 1, value, 1, ACCEL_TIMEOUT_MS);
}

esp_err_t accelerometer_write_register(const AccelSensor *sensor, uint8_t reg, uint8_t value) {
    uint8_t buf[2] = { reg, value };
    return i2c_master_transmit(sensor->dev_handle, buf, sizeof(buf), ACCEL_TIMEOUT_MS);
}

/* Verify the device identity and switch it to measurement mode. */
static esp_err_t configure_sensor(AccelSensor *sensor) {
    uint8_t devid = 0;
    if (accelerometer_read_register(sensor, ADXL345_REG_DEVID, &devid) != ESP_OK ||
        devid != ADXL345_DEVID_EXPECTED) {
        ESP_LOGW(TAG, "addr 0x%02X: unexpected DEVID 0x%02X (expected 0x%02X) — ignored",
                 sensor->address, devid, ADXL345_DEVID_EXPECTED);
        return ESP_ERR_NOT_FOUND;
    }

    /* ±2 g, 10-bit is the register default — write it explicitly for clarity. */
    accelerometer_write_register(sensor, ADXL345_REG_DATA_FORMAT, 0x00);
    accelerometer_write_register(sensor, ADXL345_REG_POWER_CTL, ADXL345_POWER_CTL_MEASURE);

    ESP_LOGI(TAG, "ADXL345 at 0x%02X ready", sensor->address);
    return ESP_OK;
}

esp_err_t accelerometer_init(void) {
    accelerometer_calibrate_load();

    accel_sensor.address    = ACCEL_ADDR_SDO_GND;
    accel_sensor.dev_handle = NULL;
    accel_sensor.present    = false;

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
        ESP_LOGE(TAG, "I2C bus init failed: %s — sensor disabled",
                 esp_err_to_name(err));
        return ESP_OK;
    }

    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = accel_sensor.address,
        .scl_speed_hz    = ACCEL_CLK_HZ,
    };

    err = i2c_master_bus_add_device(bus_handle, &dev_config,
                                    &accel_sensor.dev_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "addr 0x%02X: device handle failed: %s",
                 accel_sensor.address, esp_err_to_name(err));
        return ESP_OK;
    }

    /* Probe sends the address and checks for ACK. */
    err = i2c_master_probe(bus_handle, accel_sensor.address, ACCEL_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "addr 0x%02X: not present (%s) — sensor disabled",
                 accel_sensor.address, esp_err_to_name(err));
        return ESP_OK;
    }

    err = configure_sensor(&accel_sensor);
    accel_sensor.present = (err == ESP_OK);
    return ESP_OK;
}
