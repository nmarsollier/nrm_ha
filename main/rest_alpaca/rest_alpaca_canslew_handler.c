#include "rest_alpaca.h"
#include "rest_alpaca_internal.h"

/* Alpaca — Capability — CanSlew
 *
 * Purpose: Returns false — only the asynchronous slew is implemented
 * (SlewToCoordinatesAsync); the synchronous SlewToCoordinates is not exposed.
 *
 * Alpaca usage: N.I.N.A. checks this for slew capability reporting.
 */
esp_err_t alpaca_canslew_handler(httpd_req_t *req) {
    uint32_t cid = alpaca_get_client_transaction_id(req);
    uint32_t stx = alpaca_next_server_tx();
    alpaca_response_value(req, "true", cid, stx);
    return ESP_OK;
}
