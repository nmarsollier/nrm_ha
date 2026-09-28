#include "rest_alpaca.h"
#include "rest_alpaca_internal.h"
#include "alpaca_bridge.h"

/* Alpaca — Device — Connected (PUT)
 *
 * Purpose: tracks the client's logical link.  Conectar/desconectar no mueve.
 */
esp_err_t alpaca_connected_put_handler(httpd_req_t *req) {
    alpaca_read_body(req);
    uint32_t cid = alpaca_get_client_transaction_id(req);
    uint32_t stx = alpaca_next_server_tx();
    bool connected = false;
    if (!alpaca_get_form_bool(req, "Connected", &connected)) {
        alpaca_response_error(req, 1025, "Invalid Connected", cid, stx);
        return ESP_OK;
    }
    alpaca_bridge_set_connected(connected);
    alpaca_response_ok(req, cid, stx);
    return ESP_OK;
}
