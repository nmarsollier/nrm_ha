#pragma once

#include "esp_http_server.h"

/*
 * Test/bench debug REST endpoints.
 *
 * These routes are registered only when the firmware is built with
 * NRM_TEST_MODE defined (`idf.py -DNRM_TEST_MODE=1 build`).  In production
 * builds debug_register_routes() is a no-op, so no debug surface or test
 * logging exists on the device.
 */
void debug_register_routes(httpd_handle_t server);
