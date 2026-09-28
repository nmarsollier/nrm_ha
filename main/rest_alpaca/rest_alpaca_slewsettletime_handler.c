#include "rest_alpaca.h"
#include "rest_alpaca_internal.h"

/* Alpaca — Property — SlewSettleTime (GET)
 *
 * Purpose: Returns 0 — no post-slew settle delay is implemented.
 */
esp_err_t alpaca_slewsettletime_handler(httpd_req_t *req) {
    uint32_t cid = alpaca_get_client_transaction_id(req);
    uint32_t stx = alpaca_next_server_tx();
    alpaca_response_value(req, "0", cid, stx);
    return ESP_OK;
}
