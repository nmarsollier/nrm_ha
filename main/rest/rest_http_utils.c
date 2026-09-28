/* REST - rest_http_utils.c
 *
 * Purpose: send HTTP responses from REST handlers.
 */
#include "rest.h"
#include "motors/motors.h"

#include <stdio.h>
#include <string.h>

#include "esp_timer.h"
#include <netinet/tcp.h>
#include <lwip/sockets.h>

/*
 * Disable Nagle's algorithm on every accepted HTTP socket.
 *
 * The httpd emits a response as several small TCP segments (status line, each
 * additional header, then the body).  With Nagle enabled, each small segment is
 * held back until the previous one is ACKed, adding latency on the USB-NCM
 * link.  TCP_NODELAY sends each segment immediately.
 *
 * Registered as httpd_config_t.open_fn, so it runs on every accepted socket.
 */
esp_err_t rest_httpd_open_nodelay(httpd_handle_t hd, int sockfd) {
    (void) hd;
    int nodelay = 1;
    setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    return ESP_OK;
}

#ifdef NRM_TEST_MODE
/*
 * Echo the full motors status + timestamp back in a response header, captured
 * at the controller's entry — BEFORE the business use case runs.  A black-box
 * harness reads this header to learn the mount's exact state when the command
 * was received, so its measurements don't include the HTTP round-trip latency.
 */
void rest_set_snapshot_header(httpd_req_t *request) {
    /* static: httpd_resp_set_hdr() stores a pointer to this value, and it is
     * read back in httpd_resp_send() later in the same handler.  The httpd
     * task is single-threaded, so the value is consumed before the next call. */
    static char buf[192];
    MotorsState ms = motors_current_state();
    snprintf(buf, sizeof(buf),
             "ra=%lld;dec=%lld;st=%d;tr=%d;g=%d;ras=%.6f;decs=%.6f;t=%lld",
             (long long) ms.ra_steps, (long long) ms.dec_steps,
             (int) ms.status, (int) ms.tracking, ms.guiding ? 1 : 0,
             (double) ms.ra_speed, (double) ms.dec_speed,
             (long long) esp_timer_get_time());
    httpd_resp_set_hdr(request, "X-NRM-Snapshot", buf);
}
#endif

void http_response_json(httpd_req_t *request, const char *json) {
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Connection", "close");
    httpd_resp_sendstr(request, json);
}

void http_response_html(httpd_req_t *request, const char *html, unsigned int len) {
    httpd_resp_set_type(request, "text/html");
    httpd_resp_set_hdr(request, "Connection", "close");
    httpd_resp_send(request, html, len);
}

void http_response_bad_request(httpd_req_t *request, const char *message) {
    static const char format[] = "{\"ok\":false,\"message\":\"%s\"}";
    char response[strlen(message) + sizeof(format) + 1];

    snprintf(
        response,
        sizeof(response),
        format,
        message);

    httpd_resp_set_status(request, "400 Bad Request");
    http_response_json(request, response);
}

HttpRequestBody http_request_read_body(httpd_req_t *request) {
    HttpRequestBody body = {
        .length = 0,
        .complete = false,
        .value = {0}
    };

    int total = 0;

    if (HTTP_RESPONSE_BODY_MAX_LENGTH <= 0) {
        return body;
    }

    while (total < request->content_len && total < HTTP_RESPONSE_BODY_MAX_LENGTH - 1) {
        int received = httpd_req_recv(request, body.value + total, HTTP_RESPONSE_BODY_MAX_LENGTH - 1 - total);

        if (received <= 0) {
            break;
        }

        total += received;
    }

    body.value[total] = '\0';
    body.length = total;
    body.complete = total >= request->content_len;

    return body;
}
