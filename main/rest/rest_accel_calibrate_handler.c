/* REST - rest_accel_calibrate_handler.c
 *
 * Purpose: trigger the automatic accelerometer polar-axis calibration.
 *
 * Accepts POST /api/accel/calibrate with JSON body {"action":"start"}.
 * The calibration runs in the background (the mount slews RA by itself);
 * the UI follows its progress via the status endpoint.
 */
#include "rest.h"

#include <string.h>

#include "accelerometer.h"
#include "utils/utils.h"

esp_err_t rest_accel_calibrate_handler(httpd_req_t *request) {
    HttpRequestBody body = http_request_read_body(request);
    JsonStringResult action = json_get_string(body.value, "action");

    if (!action.ok) {
        http_response_bad_request(request,
            "Missing or invalid 'action'. Valid values: [start]");
        return ESP_OK;
    }

    if (strcmp(action.value, "start") == 0) {
        if (accelerometer_is_calibrating()) {
            rest_send_result(request, (MountResult){
                .ok = false,
                .message = "Calibration already running"
            });
            return ESP_OK;
        }

        accelerometer_calibrate_start();
        rest_send_result(request, (MountResult){
            .ok = true,
            .message = "Calibration started — keep the mount clear"
        });
        return ESP_OK;
    }

    http_response_bad_request(request, "Unknown action. Valid values: [start]");
    return ESP_OK;
}
