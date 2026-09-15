/* Accelerometer — accelerometer_calibrate.c
 *
 * Purpose: learn the polar-axis direction in the sensor frame by driving
 * the mount through known RA positions and capturing gravity at each.
 *
 * The polar axis is fixed in the sensor frame (a property of how the
 * sensor is glued to the mount).  As RA rotates, the measured gravity
 * vector traces a circle whose plane is perpendicular to the polar axis.
 * Three captures at different RA angles define that plane; its normal is
 * the polar axis.  The mount moves RA by itself, so the operator only has
 * to keep the mount clear of obstacles while it runs.
 */
#include "accelerometer_internal.h"

#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "motors/motors.h"
#include "nvs.h"
#include "utils/utils.h"

#include "esp_log.h"

static const char *TAG = "ACCELEROMETER_CALIBRATE";
static const char *NVS_NS = "mount";

#define CALIB_SWEEP_DEG          60.0f   /* max RA travel per side, clamped to limits */
#define CALIB_SPEED_RATE         3        /* 6 °/s */
#define CALIB_SETTLE_MS          1500
#define CALIB_G_TOLERANCE        0.25f    /* |g| - 1 tolerance — loose, to accept zero-g offset */
#define CALIB_MAX_SLEW_WAIT_MS   60000

/* Fitted polar-axis direction, unit vector in the sensor frame. */
static float s_polar_axis[3] = { 0.0f, 0.0f, 1.0f };
static bool  s_calibrated = false;

/* Accelerometer RA reference (home) — the φ reading at the ZERO position. */
static float s_ra_home_phi = 0.0f;
static bool  s_ra_home_set = false;

static TaskHandle_t s_calib_task = NULL;

static float vec_dot(const float *a, const float *b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

void accelerometer_calibrate_load(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }

    uint32_t v;
    float axis[3];
    bool ok = nvs_get_u32(h, "accel_ax0", &v) == ESP_OK; axis[0] = uint32_to_float(v);
    ok = ok && nvs_get_u32(h, "accel_ax1", &v) == ESP_OK; axis[1] = uint32_to_float(v);
    ok = ok && nvs_get_u32(h, "accel_ax2", &v) == ESP_OK; axis[2] = uint32_to_float(v);

    if (nvs_get_u32(h, "accel_ra_home", &v) == ESP_OK) {
        s_ra_home_phi = uint32_to_float(v);
        s_ra_home_set = true;
    }
    nvs_close(h);

    if (!ok) {
        return;
    }

    float mag = sqrtf(vec_dot(axis, axis));
    if (mag < 0.5f || mag > 1.5f) {
        ESP_LOGW(TAG, "stored polar axis invalid (mag %.2f) — ignoring", (double) mag);
        return;
    }

    s_polar_axis[0] = axis[0] / mag;
    s_polar_axis[1] = axis[1] / mag;
    s_polar_axis[2] = axis[2] / mag;
    s_calibrated = true;
    ESP_LOGI(TAG, "polar axis loaded");
}

static void save_axis(void) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open for write failed: %s", esp_err_to_name(err));
        return;
    }

    nvs_set_u32(h, "accel_ax0", float_to_uint32(s_polar_axis[0]));
    nvs_set_u32(h, "accel_ax1", float_to_uint32(s_polar_axis[1]));
    nvs_set_u32(h, "accel_ax2", float_to_uint32(s_polar_axis[2]));

    err = nvs_commit(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS commit failed: %s", esp_err_to_name(err));
    }
    nvs_close(h);
}

bool accelerometer_is_calibrating(void) {
    return s_calib_task != NULL;
}

/* Capture the current gravity vector, retrying until the sensor is still. */
static bool capture_gravity(float g[3]) {
    if (!accel_sensor.present) {
        ESP_LOGW(TAG, "capture failed: sensor not present");
        return false;
    }

    float last_mag = 0.0f;
    for (int attempt = 0; attempt < 10; attempt++) {
        AccelSample sample;
        if (accelerometer_read_sample(&accel_sensor, &sample) == ESP_OK) {
            float mag = sqrtf(sample.x_g * sample.x_g
                            + sample.y_g * sample.y_g
                            + sample.z_g * sample.z_g);
            last_mag = mag;
            if (mag > 0.05f && fabsf(mag - 1.0f) <= CALIB_G_TOLERANCE) {
                g[0] = sample.x_g / mag;
                g[1] = sample.y_g / mag;
                g[2] = sample.z_g / mag;
                return true;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    ESP_LOGW(TAG, "capture failed: last |g| = %.3f", (double) last_mag);
    return false;
}

/* Move RA by a relative amount, wait for the slew to finish, then settle. */
static bool slew_ra_and_settle(float delta_deg) {
    if (fabsf(delta_deg) < 0.001f) {
        vTaskDelay(pdMS_TO_TICKS(CALIB_SETTLE_MS));
        return true;
    }
    if (motors_slew_axis_ra(delta_deg, CALIB_SPEED_RATE) != MOTOR_OK) {
        ESP_LOGW(TAG, "RA move rejected (%.1f° — limits?)", (double) delta_deg);
        return false;
    }

    int waited_ms = 0;
    bool saw_slewing = false;
    while (waited_ms < CALIB_MAX_SLEW_WAIT_MS) {
        MotorsStatus st = motors_current_state().status;
        if (st == MOTORS_STATUS_SLEWING) {
            saw_slewing = true;
        } else if (saw_slewing) {
            break;  /* started and finished */
        }
        vTaskDelay(pdMS_TO_TICKS(50));
        waited_ms += 50;
    }

    vTaskDelay(pdMS_TO_TICKS(CALIB_SETTLE_MS));
    return true;
}

/* Fit the polar axis as the normal to the plane of the three captures. */
static void fit_and_save(float g[3][3]) {
    float d1[3] = { g[1][0] - g[0][0], g[1][1] - g[0][1], g[1][2] - g[0][2] };
    float d2[3] = { g[2][0] - g[0][0], g[2][1] - g[0][1], g[2][2] - g[0][2] };
    float axis[3] = {
        d1[1] * d2[2] - d1[2] * d2[1],
        d1[2] * d2[0] - d1[0] * d2[2],
        d1[0] * d2[1] - d1[1] * d2[0],
    };
    float mag = sqrtf(vec_dot(axis, axis));
    if (mag < 0.05f) {
        ESP_LOGW(TAG, "calibration failed: degenerate plane");
        return;
    }

    s_polar_axis[0] = axis[0] / mag;
    s_polar_axis[1] = axis[1] / mag;
    s_polar_axis[2] = axis[2] / mag;
    s_calibrated = true;
    save_axis();
    ESP_LOGI(TAG, "calibration saved");
}

static void calibration_task(void *arg) {
    (void) arg;

    float start_ra = motors_get_ra_deg();
    MotorsState ms = motors_current_state();
    float lo = fmaxf(start_ra - CALIB_SWEEP_DEG, ms.limits.ra_min);
    float hi = fminf(start_ra + CALIB_SWEEP_DEG, ms.limits.ra_max);
    float mid = (lo + hi) / 2.0f;

    float g[3][3];
    bool ok = true;

    /* Capture at three well-separated RA positions, at most ±CALIB_SWEEP_DEG. */
    ok = slew_ra_and_settle(mid - motors_get_ra_deg()) && ok;
    ok = capture_gravity(g[0]) && ok;

    ok = slew_ra_and_settle(lo - motors_get_ra_deg()) && ok;
    ok = capture_gravity(g[1]) && ok;

    ok = slew_ra_and_settle(hi - motors_get_ra_deg()) && ok;
    ok = capture_gravity(g[2]) && ok;

    /* Return to the start position (best effort). */
    motors_slew_axis_ra(start_ra - motors_get_ra_deg(), CALIB_SPEED_RATE);

    if (ok) {
        fit_and_save(g);
    } else {
        ESP_LOGW(TAG, "calibration failed — keep the mount clear and still");
    }

    s_calib_task = NULL;
    vTaskDelete(NULL);
}

void accelerometer_calibrate_start(void) {
    if (s_calib_task != NULL) {
        return;  /* already running */
    }

    BaseType_t rc = xTaskCreate(calibration_task, "accel_cal", 4096, NULL, 5, &s_calib_task);
    if (rc != pdPASS) {
        ESP_LOGE(TAG, "failed to create calibration task");
        s_calib_task = NULL;
    }
}

float accelerometer_get_elevation_deg(void) {
    if (!s_calibrated || s_calib_task != NULL) {
        return -1.0f;
    }

    AccelSample sample;
    if (!accelerometer_get_sample(&sample)) {
        return -1.0f;
    }

    float mag = sqrtf(sample.x_g * sample.x_g
                    + sample.y_g * sample.y_g
                    + sample.z_g * sample.z_g);
    if (mag < 0.05f) {
        return -1.0f;
    }

    float g[3] = { sample.x_g / mag, sample.y_g / mag, sample.z_g / mag };
    float dot = vec_dot(g, s_polar_axis);
    return asinf(fminf(fabsf(dot), 1.0f)) * ACCEL_DEG_PER_RAD;
}

/* ── RA rotation angle & limit watchdog ────────────────────── */

#define ACCEL_RA_LIMIT_MARGIN_DEG  2.0f

static float normalize_180(float a) {
    while (a > 180.0f) a -= 360.0f;
    while (a < -180.0f) a += 360.0f;
    return a;
}

/* Deterministic orthonormal basis (e1, e2) of the plane perpendicular to A. */
static bool plane_basis(float e1[3], float e2[3]) {
    float ref[3] = { 0.0f, 0.0f, 1.0f };
    float c[3] = {
        s_polar_axis[1] * ref[2] - s_polar_axis[2] * ref[1],
        s_polar_axis[2] * ref[0] - s_polar_axis[0] * ref[2],
        s_polar_axis[0] * ref[1] - s_polar_axis[1] * ref[0],
    };
    float mag = sqrtf(vec_dot(c, c));
    if (mag < 0.1f) {
        ref[0] = 1.0f; ref[1] = 0.0f; ref[2] = 0.0f;
        c[0] = s_polar_axis[1] * ref[2] - s_polar_axis[2] * ref[1];
        c[1] = s_polar_axis[2] * ref[0] - s_polar_axis[0] * ref[2];
        c[2] = s_polar_axis[0] * ref[1] - s_polar_axis[1] * ref[0];
        mag = sqrtf(vec_dot(c, c));
    }
    if (mag < 0.05f) {
        return false;
    }
    e1[0] = c[0] / mag; e1[1] = c[1] / mag; e1[2] = c[2] / mag;
    e2[0] = s_polar_axis[1] * e1[2] - s_polar_axis[2] * e1[1];
    e2[1] = s_polar_axis[2] * e1[0] - s_polar_axis[0] * e1[2];
    e2[2] = s_polar_axis[0] * e1[1] - s_polar_axis[1] * e1[0];
    return true;
}

/*
 * RA rotation angle measured by the accelerometer, 0-360°, independent of
 * the motor step counters.  Returns a negative value when unavailable
 * (uncalibrated or no reading yet).
 */
float accelerometer_get_ra_angle_deg(void) {
    if (!s_calibrated) {
        return -1.0f;
    }

    AccelSample sample;
    if (!accelerometer_get_sample(&sample)) {
        return -1.0f;
    }

    float mag = sqrtf(sample.x_g * sample.x_g
                    + sample.y_g * sample.y_g
                    + sample.z_g * sample.z_g);
    if (mag < 0.05f || fabsf(mag - 1.0f) > ACCEL_G_TOLERANCE) {
        return -1.0f;  /* degenerate or vibrating — not a clean gravity reading */
    }

    float g[3] = { sample.x_g / mag, sample.y_g / mag, sample.z_g / mag };
    float dot = vec_dot(g, s_polar_axis);
    float g_perp[3] = {
        g[0] - dot * s_polar_axis[0],
        g[1] - dot * s_polar_axis[1],
        g[2] - dot * s_polar_axis[2],
    };

    float e1[3], e2[3];
    if (!plane_basis(e1, e2)) {
        return -1.0f;
    }

    float u = vec_dot(g_perp, e1);
    float v = vec_dot(g_perp, e2);
    float phi = atan2f(v, u) * ACCEL_DEG_PER_RAD;
    if (phi < 0.0f) {
        phi += 360.0f;
    }
    return phi;
}

/* Record the current RA angle as the home (0°) reference. */
void accelerometer_set_ra_home(void) {
    if (!s_calibrated) {
        return;
    }
    float phi = accelerometer_get_ra_angle_deg();
    if (phi < 0.0f) {
        return;
    }

    s_ra_home_phi = phi;
    s_ra_home_set = true;

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, "accel_ra_home", float_to_uint32(phi));
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "RA home set at %.1f°", (double) phi);
}

/*
 * Accelerometer RA angle relative to the home reference, signed (-180..180),
 * so 0° at home and directly comparable to motors_get_ra_deg().  Returns a
 * large negative sentinel when unavailable.
 */
float accelerometer_get_ra_signed_deg(void) {
    if (!s_calibrated || !s_ra_home_set) {
        return -1000.0f;
    }

    float phi = accelerometer_get_ra_angle_deg();
    if (phi < 0.0f) {
        return -1000.0f;
    }
    return normalize_180(phi - s_ra_home_phi);
}

/*
 * True when the accelerometer RA angle is outside the motor limits (plus a
 * small margin).  The caller (motion task) decides how to stop.  Returns
 * false when the accelerometer is uncalibrated or has no reference.
 */
bool accelerometer_ra_limit_exceeded(void) {
    if (!s_calibrated || !s_ra_home_set) {
        return false;
    }

    float phi = accelerometer_get_ra_angle_deg();
    if (phi < 0.0f) {
        return false;
    }

    float ra_accel = normalize_180(phi - s_ra_home_phi);
    MotorsState ms = motors_current_state();
    float lo = ms.limits.ra_min - ACCEL_RA_LIMIT_MARGIN_DEG;
    float hi = ms.limits.ra_max + ACCEL_RA_LIMIT_MARGIN_DEG;
    return ra_accel < lo || ra_accel > hi;
}
