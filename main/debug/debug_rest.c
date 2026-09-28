/* Debug - debug_rest.c
 *
 * Purpose: test/bench-only REST endpoints that let a black-box test harness
 * reach internal error conditions that are impossible to trigger through the
 * public mount API (fake power loss, latched hardware fault).
 *
 * Compiled and registered ONLY under NRM_TEST_MODE.
 */
#include "debug.h"
#include "rest.h"
#include "rest_internal.h"

#include "power.h"
#include "motors.h"

#include "esp_log.h"
#include <string.h>

static const char *TAG = "DEBUG_REST";

#ifdef NRM_TEST_MODE

/*
 * POST /api/debug/power  {"force":0|1, "present":0|1}
 *   force=1  → force the external-power reading to `present`.
 *   force=0  → clear the override and fall back to the ADC.
 */
static esp_err_t debug_power_handler(httpd_req_t *request) {
    HttpRequestBody body = http_request_read_body(request);
    JsonIntResult force = json_get_int(body.value, "force");
    JsonIntResult present = json_get_int(body.value, "present");

    if (!force.ok) {
        http_response_bad_request(request, "Missing or invalid 'force' (0|1)");
        return ESP_OK;
    }
    if (force.value && !present.ok) {
        http_response_bad_request(request, "Missing or invalid 'present' (0|1)");
        return ESP_OK;
    }

    if (force.value) {
        power_debug_force(true, present.value != 0);
        ESP_LOGI(TAG, "debug power forced present=%d", present.value);
    } else {
        power_debug_force(false, false);
        ESP_LOGI(TAG, "debug power override cleared (back to ADC)");
    }

    http_response_json(request, "{\"ok\":true,\"message\":\"OK\"}");
    return ESP_OK;
}

/*
 * POST /api/debug/fault  {"type":"hardware"}
 *   Inject a latched hardware fault.  Only a reboot clears it.
 */
static esp_err_t debug_fault_handler(httpd_req_t *request) {
    HttpRequestBody body = http_request_read_body(request);
    JsonStringResult type = json_get_string(body.value, "type");

    if (type.ok && strcmp(type.value, "hardware") == 0) {
        motors_debug_force_hardware_fault();
        ESP_LOGI(TAG, "debug injected latched hardware fault");
        http_response_json(request, "{\"ok\":true,\"message\":\"OK\"}");
    } else {
        http_response_bad_request(request, "Missing or invalid 'type' (only 'hardware')");
    }
    return ESP_OK;
}

void debug_register_routes(httpd_handle_t server) {
    rest_register_post(server, "/api/debug/power", debug_power_handler);
    rest_register_post(server, "/api/debug/fault", debug_fault_handler);
    ESP_LOGI(TAG, "debug routes registered (TEST MODE)");
}

#else  /* !NRM_TEST_MODE */

void debug_register_routes(httpd_handle_t server) {
    (void) server;
}

#endif
