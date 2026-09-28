/* Rest - rest_reset_handler.c
 *
 * Purpose: reboot the mount controller firmware.
 */
#include "rest.h"

#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/*
 * Business use case: expose RESET as an API command.
 *
 * Objective: restart the ESP32 firmware — the only way to recover from the
 * mount's ERROR state — acknowledging the request before rebooting.
 */
esp_err_t rest_reset_handler(httpd_req_t *request) {
    /* Stop the motion before rebooting so the reset is a confirmed stop,
     * not a power-cut that leaves a step burst in flight. */
    mount_stop();

    MountResult result = { .ok = true, .message = "Rebooting" };
    rest_send_result(request, result);

    /* Let the ack flush to the client before the chip resets. */
    vTaskDelay(pdMS_TO_TICKS(200));

    esp_restart();
    return ESP_OK;
}
