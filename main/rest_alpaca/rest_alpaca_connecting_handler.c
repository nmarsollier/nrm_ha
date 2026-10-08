/* Alpaca — Device — Connecting
 *
 * Purpose: reports whether a connection is being established.  This mount's
 * connect/disconnect is instantaneous (the Connected flag flips immediately),
 * so Connecting is always false.
 *
 * Telescope Interface V4 property.
 */
#include "rest_alpaca.h"
#include "rest_alpaca_internal.h"

esp_err_t alpaca_connecting_handler(httpd_req_t *req) {
    uint32_t cid = alpaca_get_client_transaction_id(req);
    uint32_t stx = alpaca_next_server_tx();
    alpaca_response_value(req, "false", cid, stx);
    return ESP_OK;
}
