#pragma once

#include "esp_http_server.h"
#include "mount.h"

/*
 * Physical motor axis identifiers used by REST API endpoints that accept
 * an axis parameter (e.g. MoveAxis).
 */
typedef enum {
    MOTOR_AXIS_RA,
    MOTOR_AXIS_DEC,
    MOTOR_AXIS_UNKNOWN
} MotorAxis;

/* HTTP response helpers. */
#define HTTP_RESPONSE_BODY_MAX_LENGTH 512

typedef struct {
    int length;
    bool complete;
    char value[HTTP_RESPONSE_BODY_MAX_LENGTH];
} HttpRequestBody;

void http_response_json(httpd_req_t *request, const char *json);

void http_response_html(httpd_req_t *request, const char *html, unsigned int len);

void http_response_bad_request(httpd_req_t *request, const char *message);

HttpRequestBody http_request_read_body(httpd_req_t *request);

void rest_server_start(void);

esp_err_t rest_status_handler(httpd_req_t *request);

esp_err_t rest_html_handler(httpd_req_t *request);

esp_err_t rest_tracking_handler(httpd_req_t *request);

esp_err_t rest_slew_to_coordinates_handler(httpd_req_t *request);

esp_err_t rest_move_axis_handler(httpd_req_t *request);
esp_err_t rest_move_axis_speed_handler(httpd_req_t *request);

esp_err_t rest_stop_handler(httpd_req_t *request);

esp_err_t rest_reset_handler(httpd_req_t *request);

esp_err_t rest_park_handler(httpd_req_t *request);

esp_err_t rest_unpark_handler(httpd_req_t *request);

esp_err_t rest_home_handler(httpd_req_t *request);

esp_err_t rest_settings_handler(httpd_req_t *request);

esp_err_t rest_limits_handler(httpd_req_t *request);

void rest_send_result(
    httpd_req_t *request,
    MountResult result);

/* Route registration helpers — shared with the Alpaca server. */
void rest_register_get(httpd_handle_t server, const char *uri,
                       esp_err_t (*handler)(httpd_req_t *));

void rest_register_post(httpd_handle_t server, const char *uri,
                        esp_err_t (*handler)(httpd_req_t *));

void rest_register_put(httpd_handle_t server, const char *uri,
                       esp_err_t (*handler)(httpd_req_t *));

/* Axis string helpers for REST API parameter parsing. */
MotorAxis rest_axis_from_string(const char *value);
const char *rest_axis_valid_values(void);
