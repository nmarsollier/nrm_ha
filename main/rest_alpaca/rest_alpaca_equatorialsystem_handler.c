#include "rest_alpaca.h"
#include "rest_alpaca_internal.h"
#include <stdio.h>

/* Alpaca — Property — EquatorialSystem
 *
 * Purpose: Returns equTopocentric (1) — the coordinate system used.
 *
 * The mount works in "of date" coordinates: RA/DEC are interpreted against the
 * current date's mean equinox (GMST-based; no precession, nutation or
 * aberration).  Declaring J2000 (2) here would make the client send catalog
 * epoch coordinates that the mount would then misread by the ~precession
 * offset, so the honest answer is the topocentric "now" frame.
 *
 * Alpaca usage: N.I.N.A. uses this to pick JNow vs J2000 mode.
 */
esp_err_t alpaca_equatorialsystem_handler(httpd_req_t *req) {
    uint32_t cid = alpaca_get_client_transaction_id(req);
    uint32_t stx = alpaca_next_server_tx();
    int result = 1;
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", result);
    alpaca_response_value(req, buf, cid, stx);
    return ESP_OK;
}
