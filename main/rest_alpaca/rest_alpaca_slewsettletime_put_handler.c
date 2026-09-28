#include "rest_alpaca.h"
#include "rest_alpaca_internal.h"

/* Alpaca — Property — SlewSettleTime (PUT)
 *
 * Purpose: Not implemented — the mount has no configurable settle time.
 */
esp_err_t alpaca_slewsettletime_put_handler(httpd_req_t *req) {
    uint32_t cid = alpaca_get_client_transaction_id(req);
    uint32_t stx = alpaca_next_server_tx();
    alpaca_response_error(req, 1024, "SlewSettleTime is not supported", cid, stx);
    return ESP_OK;
}
