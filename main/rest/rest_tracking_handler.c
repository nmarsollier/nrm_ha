/* REST - rest_tracking_handler.c
 *
 * Purpose: handle tracking mode changes.
 */
#include "rest.h"

#include <stdio.h>
#include <string.h>

#include "mount.h"

#include "rest_internal.h"

/*
 * Business use case: expose tracking changes via the API.
 *
 * Objective: let clients select the tracking mode for the current target and
 * observing strategy.
 */
esp_err_t rest_tracking_handler(httpd_req_t *request) {
    HttpRequestBody body = http_request_read_body(request);
    JsonStringResult tracking_text = json_get_string(body.value, "tracking");

    /* motors_tracking_from_string() maps an unknown value to TRACKING_NONE,
     * so "bogus" would be silently accepted as a stop.  Validate the value
     * against the known modes before parsing. */
    const char *v = tracking_text.value;
    bool valid = tracking_text.ok
        && (strcmp(v, "none") == 0 || strcmp(v, "sidereal") == 0
            || strcmp(v, "lunar") == 0 || strcmp(v, "solar") == 0);

    if (!valid) {
        static const char format[] = "Missing or invalid 'tracking'. Valid values: %s";
        const char *valid_values = motors_tracking_valid_values();

        char message[strlen(valid_values) + sizeof(format)];
        snprintf(message, sizeof(message), format, valid_values);

        http_response_bad_request(request, message);
        return ESP_OK;
    }

    rest_send_result(request, mount_set_tracking(motors_tracking_from_string(v)));

    return ESP_OK;
}
