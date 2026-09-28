#include "rest_alpaca.h"
#include "rest_alpaca_internal.h"

/* Alpaca — Property — SideOfPier (PUT)
 *
 * Purpose: Not implemented — pier side is derived from the mechanical
 * position and cannot be set directly.  CanSetPierSide returns false.
 */
esp_err_t alpaca_sideofpier_put_handler(httpd_req_t *req) {
    uint32_t cid = alpaca_get_client_transaction_id(req);
    uint32_t stx = alpaca_next_server_tx();
    alpaca_response_error(req, 1024, "SideOfPier is read-only", cid, stx);
    return ESP_OK;
}
