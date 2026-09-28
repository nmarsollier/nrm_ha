/* REST - rest_stop_handler.c
 *
 * Purpose: handle stop requests.
 */
#include "rest.h"

#include "mount.h"

/*
 * Business use case: expose STOP as an API command.
 *
 * Objective: stop any ongoing movement safely and predictably.
 */
esp_err_t rest_stop_handler(httpd_req_t *request) {
#ifdef NRM_TEST_MODE
    /* Echo the mount state + timestamp captured before the STOP business call,
     * so a harness can measure post-STOP movement without the HTTP round trip. */
    rest_set_snapshot_header(request);
#endif

    rest_send_result(request, mount_stop());

    return ESP_OK;
}
