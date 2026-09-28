#include "rest_alpaca.h"
#include "rest_alpaca_internal.h"

/* Alpaca — Capability — CanSetPark
 *
 * Purpose: Returns false — the park position cannot be set.
 *
 * Park is a fixed "stop and mark PARKED" operation; there is no configurable
 * park position, so SetPark is not implemented.
 */
esp_err_t alpaca_cansetpark_handler(httpd_req_t *req) {
    uint32_t cid = alpaca_get_client_transaction_id(req);
    uint32_t stx = alpaca_next_server_tx();
    alpaca_response_value(req, "false", cid, stx);
    return ESP_OK;
}
