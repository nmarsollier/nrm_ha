#include "rest_alpaca_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "esp_http_server.h"
#include "esp_log.h"

#define ALPACA_RESPONSE_BUFFER 512

static const char *TAG = "ALPACA_HELPERS";

static uint32_t s_server_tx = 0;

uint32_t alpaca_next_server_tx(void) {
    return ++s_server_tx;
}

/* ─── Internal helper ─── */

static void alpaca_send_json(httpd_req_t *req, const char *json) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, json, strlen(json));
}

/* ─── Public API ─── */

void alpaca_response_value(httpd_req_t *req, const char *value_json,
                           uint32_t client_transaction_id, uint32_t server_tx) {
    char buf[ALPACA_RESPONSE_BUFFER];
    snprintf(buf, sizeof(buf),
             "{\"Value\":%s,\"ClientTransactionID\":%lu,"
             "\"ServerTransactionID\":%lu,\"ErrorNumber\":0,"
             "\"ErrorMessage\":\"\"}",
             value_json, (unsigned long) client_transaction_id, (unsigned long) server_tx);
    alpaca_send_json(req, buf);
}

void alpaca_response_value_dynamic(httpd_req_t *req, const char *value_json,
                                   uint32_t client_transaction_id, uint32_t server_tx) {
    /* Envelope text + two up-to-10-digit transaction IDs + NUL. */
    size_t need = strlen(value_json) + 128;
    char *buf = malloc(need);
    if (!buf) {
        alpaca_response_error(req, 0x500, "Out of memory",
                              client_transaction_id, server_tx);
        return;
    }
    snprintf(buf, need,
             "{\"Value\":%s,\"ClientTransactionID\":%lu,"
             "\"ServerTransactionID\":%lu,\"ErrorNumber\":0,"
             "\"ErrorMessage\":\"\"}",
             value_json, (unsigned long) client_transaction_id, (unsigned long) server_tx);
    alpaca_send_json(req, buf);
    free(buf);
}

void alpaca_response_ok(httpd_req_t *req,
                        uint32_t client_transaction_id, uint32_t server_tx) {
    char buf[ALPACA_RESPONSE_BUFFER];
    snprintf(buf, sizeof(buf),
             "{\"ClientTransactionID\":%lu,"
             "\"ServerTransactionID\":%lu,\"ErrorNumber\":0,"
             "\"ErrorMessage\":\"\"}",
             (unsigned long) client_transaction_id, (unsigned long) server_tx);
    alpaca_send_json(req, buf);
}

void alpaca_response_error(httpd_req_t *req, int error_number,
                           const char *message,
                           uint32_t client_transaction_id, uint32_t server_tx) {
    char buf[ALPACA_RESPONSE_BUFFER];
    snprintf(buf, sizeof(buf),
             "{\"ClientTransactionID\":%lu,"
             "\"ServerTransactionID\":%lu,"
             "\"ErrorNumber\":%d,"
             "\"ErrorMessage\":\"%s\"}",
             (unsigned long) client_transaction_id, (unsigned long) server_tx,
             error_number, message);
    alpaca_send_json(req, buf);
}

/* ─── Parameter parsing ─── */

/*
 * Request body buffer — read once per request by the handler.
 * Multiple form-param reads operate on this buffer, avoiding the
 * "body consumed" issue with httpd_req_recv.
 *
 * N.B. ESP-IDF reuses httpd_req_t objects so pointer-based caching
 * is NOT safe across requests — the handler MUST call alpaca_read_body
 * at the top of each handler that uses form params.
 */
static char  s_body_buf[512];
static int   s_body_len = 0;

/*
 * URL-decode a percent-encoded string in-place.
 * "2026-06-13T13%3A53%3A04" → "2026-06-13T13:53:04"
 * Returns the new (shorter) length.
 */
static size_t url_decode_inplace(char *s, size_t len) {
    size_t w = 0;
    for (size_t r = 0; r < len; r++, w++) {
        if (s[r] == '%' && r + 2 < len) {
            char hex[3] = {s[r + 1], s[r + 2], '\0'};
            char *end;
            long c = strtol(hex, &end, 16);
            if (end == hex + 2) {
                s[w] = (char) c;
                r += 2;
                continue;
            }
        }
        if (s[r] == '+') {
            /* form-encoding: '+' is a space. */
            s[w] = ' ';
            continue;
        }
        s[w] = s[r];
    }
    if (w < len) s[w] = '\0';
    return w;
}

void alpaca_read_body(httpd_req_t *req) {
    s_body_len = 0;
    s_body_buf[0] = '\0';

    /* content_len is 0 when there is no body — avoids a blocking recv. */
    int content_len = (int) req->content_len;
    if (content_len <= 0) {
        return;
    }

    /* Reject an oversized body instead of silently truncating it. */
    if (content_len > (int)(sizeof(s_body_buf) - 1)) {
        ESP_LOGW(TAG, "body too large (%d > %d)", content_len,
                 (int)(sizeof(s_body_buf) - 1));
        return;
    }

    /* Read until the declared length is consumed — TCP may fragment. */
    int total = 0;
    while (total < content_len) {
        int ret = httpd_req_recv(req, s_body_buf + total, content_len - total);
        if (ret <= 0) break;
        total += ret;
    }
    s_body_len = total;
    s_body_buf[s_body_len] = '\0';
}

const char *alpaca_dump_body(httpd_req_t *req, int *out_len) {
    (void) req;
    if (out_len) *out_len = s_body_len;
    return (s_body_len > 0) ? s_body_buf : "";
}

char *alpaca_get_form_param(httpd_req_t *req, const char *key) {
    (void) req;
    if (s_body_len <= 0) return NULL;

    size_t key_len = strlen(key);
    const char *p = s_body_buf;
    while (*p) {
        if (strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
            p += key_len + 1;
            const char *val_end = p;
            while (*val_end && *val_end != '&') val_end++;
            size_t vlen = val_end - p;
            char *value = malloc(vlen + 1);
            if (value) {
                memcpy(value, p, vlen);
                value[vlen] = '\0';
                /* URL-decode percent-encoded characters (e.g. %3A → :) */
                url_decode_inplace(value, vlen);
            }
            return value;
        }
        while (*p && *p != '&') p++;
        if (*p == '&') p++;
    }
    return NULL;
}

/*
 * Try to read a parameter value from the query string, falling back to
 * the URL-encoded form body.  The Alpaca spec allows both; N.I.N.A. uses
 * the query string for MoveAxis and similar methods.
 */
static char *alpaca_get_param_str(httpd_req_t *req, const char *key) {
    /* 1. Query string */
    char qbuf[128];
    if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) == ESP_OK) {
        char val[64];
        if (httpd_query_key_value(qbuf, key, val, sizeof(val)) == ESP_OK) {
            return strdup(val);
        }
    }

    /* 2. Form body */
    return alpaca_get_form_param(req, key);
}

/*
 * Extract the ClientTransactionID — echoed back in the response — from the
 * query string (GET) or the form body (PUT).  Strictly parsed: returns 0 if
 * absent or malformed.  Distinct from ClientID, which is not part of the
 * response contract.
 */
uint32_t alpaca_get_client_transaction_id(httpd_req_t *req) {
    char *val = alpaca_get_param_str(req, "ClientTransactionID");
    if (!val) return 0;
    char *end = NULL;
    /* strtoul, not strtol: ClientTransactionID is a uint32, and a 32-bit
     * signed long overflows at 2147483647 (found by B-04 on hardware). */
    unsigned long v = strtoul(val, &end, 10);
    uint32_t result = (end != val && *end == '\0') ? (uint32_t) v : 0;
    free(val);
    return result;
}

bool alpaca_get_form_float(httpd_req_t *req, const char *key, float *out) {
    char *val = alpaca_get_param_str(req, key);
    if (!val) return false;
    char *end = NULL;
    float v = strtof(val, &end);
    bool ok = (end != val) && isfinite(v) && (*end == '\0');
    if (ok) *out = v;
    free(val);
    return ok;
}

bool alpaca_get_form_bool(httpd_req_t *req, const char *key, bool *out) {
    char *val = alpaca_get_param_str(req, key);
    if (!val) return false;
    bool ok;
    if (strcasecmp(val, "true") == 0 || strcmp(val, "1") == 0) {
        *out = true;
        ok = true;
    } else if (strcasecmp(val, "false") == 0 || strcmp(val, "0") == 0) {
        *out = false;
        ok = true;
    } else {
        /* "bogus" is not a boolean: reject it, don't assume false. */
        ok = false;
    }
    free(val);
    return ok;
}

bool alpaca_get_form_int(httpd_req_t *req, const char *key, int *out) {
    char *val = alpaca_get_param_str(req, key);
    if (!val) return false;
    char *end = NULL;
    long v = strtol(val, &end, 10);
    bool ok = (end != val) && (*end == '\0');
    if (ok) *out = (int) v;
    free(val);
    return ok;
}
