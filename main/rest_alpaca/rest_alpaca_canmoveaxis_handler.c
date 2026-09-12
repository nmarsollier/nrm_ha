#include "rest_alpaca.h"
#include "rest_alpaca_internal.h"
#include "motors/motors.h"

/* Alpaca — Capability — CanMoveAxis
 *
 * Purpose: Returns whether an axis supports manual movement.  Equatorial
 * mount: RA (0) and DEC (1) yes, Tertiary (2) no.
 *
 * Alpaca usage: N.I.N.A. enables manual motion controls for RA and DEC.
 */
esp_err_t alpaca_canmoveaxis_handler(httpd_req_t *req) {
    alpaca_read_body(req);
    uint32_t cid = alpaca_get_client_id(req);
    uint32_t stx = alpaca_next_server_tx();

    int axis = 0;
    if (!alpaca_get_form_int(req, "Axis", &axis)) {
        alpaca_response_error(req, 1025, "Missing Axis", cid, stx);
        return ESP_OK;
    }

    bool supported = (axis == 0 || axis == 1)
                     && !motors_status_is_error(motors_current_state().status);
    alpaca_response_value(req, supported ? "true" : "false", cid, stx);
    return ESP_OK;
}
