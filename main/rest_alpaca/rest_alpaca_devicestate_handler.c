/* Alpaca — Device — DeviceState
 *
 * Purpose: returns every Telescope operational property in a single call
 * (ASCOM Telescope Interface V4 "read all" feature).  The response Value is a
 * JSON array of {"Name":..., "Value":...} objects, so a client can refresh its
 * whole view of the mount with one request instead of polling each property.
 */
#include "rest_alpaca.h"
#include "rest_alpaca_internal.h"
#include "alpaca_bridge.h"

#include <stdio.h>

esp_err_t alpaca_devicestate_handler(httpd_req_t *req) {
    uint32_t cid = alpaca_get_client_transaction_id(req);
    uint32_t stx = alpaca_next_server_tx();

    AlpacaDeviceState s;
    alpaca_bridge_get_device_state(&s);

    char buf[1024];
    int n = snprintf(buf, sizeof(buf),
        "["
        "{\"Name\":\"Altitude\",\"Value\":%.6f},"
        "{\"Name\":\"AtHome\",\"Value\":%s},"
        "{\"Name\":\"AtPark\",\"Value\":%s},"
        "{\"Name\":\"Azimuth\",\"Value\":%.6f},"
        "{\"Name\":\"Declination\",\"Value\":%.6f},"
        "{\"Name\":\"IsPulseGuiding\",\"Value\":%s},"
        "{\"Name\":\"RightAscension\",\"Value\":%.6f},"
        "{\"Name\":\"SideOfPier\",\"Value\":%d},"
        "{\"Name\":\"Slewing\",\"Value\":%s},"
        "{\"Name\":\"Tracking\",\"Value\":%s}"
        "]",
        (double) s.altitude,
        s.at_home ? "true" : "false",
        s.at_park ? "true" : "false",
        (double) s.azimuth,
        (double) s.declination,
        s.is_pulse_guiding ? "true" : "false",
        (double) s.right_ascension,
        s.side_of_pier,
        s.slewing ? "true" : "false",
        s.tracking ? "true" : "false");

    /* Should not happen for ten entries, but fall back to an empty list
     * ("best endeavours") rather than sending a truncated array. */
    if (n < 0 || n >= (int) sizeof(buf)) {
        alpaca_response_value(req, "[]", cid, stx);
        return ESP_OK;
    }

    alpaca_response_value_dynamic(req, buf, cid, stx);
    return ESP_OK;
}
