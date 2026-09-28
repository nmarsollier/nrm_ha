#include "rest_alpaca.h"
#include "rest_alpaca_internal.h"
#include "mount.h"

/* Alpaca — Method — FindHome
 *
 * Purpose: Starts the home-finding sequence.  Asynchronous — the client polls
 * AtHome until the mount reaches (0,0).
 *
 * Alpaca usage: Called by N.I.N.A. at session start or when the mount needs re-homing.
 */
esp_err_t alpaca_findhome_handler(httpd_req_t *req) {
    uint32_t cid = alpaca_get_client_transaction_id(req);
    uint32_t stx = alpaca_next_server_tx();
    MountResult result = mount_home();
    if (result.ok) alpaca_response_ok(req, cid, stx);
    else alpaca_response_error(req, 1025, result.message, cid, stx);
    return ESP_OK;
}
