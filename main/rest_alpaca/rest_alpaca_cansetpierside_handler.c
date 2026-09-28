#include "rest_alpaca.h"
#include "rest_alpaca_internal.h"

/* Alpaca — Capability — CanSetPierSide
 *
 * Purpose: Returns false — pier side cannot be set by the client.
 *
 * Changing pier side would require choosing and executing the *other*
 * mechanical solution (a flip); this mount has no such operation, so the
 * capability is honestly disabled rather than advertising a no-op setter.
 */
esp_err_t alpaca_cansetpierside_handler(httpd_req_t *req) {
    uint32_t cid = alpaca_get_client_transaction_id(req);
    uint32_t stx = alpaca_next_server_tx();
    alpaca_response_value(req, "false", cid, stx);
    return ESP_OK;
}
