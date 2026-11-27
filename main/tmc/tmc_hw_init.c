/* TMC — tmc_hw_init.c
 *
 * Purpose: install the two dedicated single-wire UART buses and configure the
 * drivers.  Must be called before any step/direction motion is started.
 */

#include "tmc.h"
#include "tmc_internal.h"

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_log.h"

static const char *TAG = "TMC_HW_INIT";

esp_err_t tmc2209_hw_init(uint16_t microsteps)
{
    if (tmc_uart_lock() != ESP_OK) {
        return ESP_FAIL;
    }

    for (size_t i = 0; i < TMC_AXIS_COUNT; i++) {
        tmc_uart_ok[i] = false;
    }

    /* Install and configure each driver's dedicated single-wire UART. */
    for (size_t i = 0; i < TMC_AXIS_COUNT; i++) {
        const AxisHardwareProfile *a = &mount_profile.hardware.axes[i];
        uart_port_t uart = (uart_port_t) a->uart_num;
        gpio_num_t tx_gpio = (gpio_num_t) a->uart_tx_gpio;
        gpio_num_t rx_gpio = (gpio_num_t) a->uart_rx_gpio;

        uart_config_t config = {
            .baud_rate  = TMC_BAUD_RATE,
            .data_bits  = UART_DATA_8_BITS,
            .parity     = UART_PARITY_DISABLE,
            .stop_bits  = UART_STOP_BITS_1,
            .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
        };

        esp_err_t result = uart_driver_install(uart, 512, 512, 0, NULL, 0);
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "%s: uart_driver_install failed: %s",
                     tmc_axis_name(i), esp_err_to_name(result));
            continue;
        }

        result = uart_param_config(uart, &config);
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "%s: uart_param_config failed: %s",
                     tmc_axis_name(i), esp_err_to_name(result));
            continue;
        }

        /* Single-wire: TX through 1 kΩ, RX direct to the PDN_UART line. */
        result = uart_set_pin(uart, tx_gpio, rx_gpio,
                              UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "%s: uart_set_pin failed: %s",
                     tmc_axis_name(i), esp_err_to_name(result));
            continue;
        }

        gpio_set_pull_mode(rx_gpio, GPIO_PULLUP_ONLY);
        tmc_uart_ok[i] = true;
    }

    esp_err_t result = tmc_configure_drivers(microsteps);
    tmc_uart_unlock();
    return result;
}
