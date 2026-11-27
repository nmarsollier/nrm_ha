/* Mount - mount_pulse_guide.c
 *
 * Purpose: validate mount state for PulseGuide, map astronomical
 * directions (North/South/East/West) to signed RA/DEC speed offsets,
 * and delegate to the motors layer.
 *
 * Direction mapping (offset added to base tracking speed):
 *   East  → +RA  (move scope east relative to sky)
 *   West  → -RA  (move scope west relative to sky)
 *   North → +DEC
 *   South → -DEC
 */
#include "mount.h"
#include "mount_internal.h"

#include "motors.h"
#include "mount_profile.h"

#include <math.h>

#include "esp_log.h"

static const char *TAG = "MOUNT_PULSE_GUIDE";

MountResult mount_pulse_guide(GuideDirection direction, uint32_t duration_ms) {
    if (mount_is_error()) {
        return mount_result_error_state();
    }

    /* PulseGuide is only valid in READY (calibration) or TRACKING (guiding). */
    MotorsState state = motors_current_state();
    if (state.status != MOTORS_STATUS_READY &&
        state.status != MOTORS_STATUS_TRACKING) {
        ESP_LOGW(TAG, "Rejected: mount status=%s",
                 motors_status_to_string(state.status));
        return mount_result_error("Mount not ready for PulseGuide");
    }

    if (duration_ms == 0 || duration_ms > 30000) {
        return mount_result_error("Duration out of range (1-30000 ms)");
    }

    float ra_rate = mount_get_guide_rate_ra();
    float dec_rate = mount_get_guide_rate_dec();

    MotorResultCode rc;
    switch (direction) {
    case GUIDE_DIRECTION_EAST:
        /* +mechanical RA reduces celestial RA (RA = LST - HA, HA grows with
         * RA axis), i.e. it moves west.  So east is -mechanical RA.
         * See axis_to_equatorial(). */
        if (ra_rate <= 0.0f) {
            ESP_LOGW(TAG, "Rejected: RA guide rate is zero (guiding disabled)");
            return mount_result_error("RA guide rate is zero — guiding disabled");
        }
        rc = motors_pulse_guide_start(0, -ra_rate, duration_ms);
        break;
    case GUIDE_DIRECTION_WEST:
        if (ra_rate <= 0.0f) {
            ESP_LOGW(TAG, "Rejected: RA guide rate is zero (guiding disabled)");
            return mount_result_error("RA guide rate is zero — guiding disabled");
        }
        rc = motors_pulse_guide_start(0,  ra_rate, duration_ms);
        break;
    case GUIDE_DIRECTION_NORTH:
    case GUIDE_DIRECTION_SOUTH: {
        if (dec_rate <= 0.0f) {
            ESP_LOGW(TAG, "Rejected: DEC guide rate is zero (guiding disabled)");
            return mount_result_error("DEC guide rate is zero — guiding disabled");
        }

        float dec_axis_deg = motors_get_dec_deg();
        float north_sign;
        if (mount_profile.geometry == GEOMETRY_FORK) {
            /* Fork: the mechanical DEC sign is the derivative dδ/dD at the
             * current position = -p·sign(sin D), p = +1 north / -1 south. */
            float p = (mount_internal_state.lat > 0.0f) ? 1.0f : -1.0f;
            float sD = sinf(dec_axis_deg * (float) M_PI / 180.0f);
            north_sign = (sD >= 0.0f ? 1.0f : -1.0f) * (-p);
        } else {
            /* German: North (increasing celestial DEC) maps to +mechanical DEC
             * on the east side of the pier (dec_axis >= 0) and to -mechanical
             * DEC on the west side (dec_axis < 0). */
            north_sign = (dec_axis_deg >= 0.0f) ? 1.0f : -1.0f;
        }
        float offset = (direction == GUIDE_DIRECTION_NORTH)
                       ?  dec_rate * north_sign
                       : -dec_rate * north_sign;

        if (mount_profile.geometry == GEOMETRY_FORK) {
            /* Reject a pulse whose signed DEC excursion reaches or crosses a
             * celestial pole (D = 180°·n), or would exceed the mechanical
             * limit — a truncated move must be a clear rejection. */
            float arc_deg = offset * (duration_ms / 1000.0f);
            float pole_eps = motors_steps_to_deg(1);   /* one microstep */
            float d_start = dec_axis_deg;
            float d_end = dec_axis_deg + arc_deg;
            float a = fminf(d_start, d_end) - pole_eps;
            float b = fmaxf(d_start, d_end) + pole_eps;
            int n_lo = (int) ceilf(a / 180.0f);
            int n_hi = (int) floorf(b / 180.0f);
            if (n_lo <= n_hi) {
                ESP_LOGW(TAG, "Rejected: DEC pulse crosses the celestial pole");
                return mount_result_error("Guide would cross the pole");
            }
            if (d_end > state.limits.dec_max || d_end < state.limits.dec_min) {
                ESP_LOGW(TAG, "Rejected: DEC guide would exceed the limit");
                return mount_result_error("Guide would exceed the DEC limit");
            }
        }

        rc = motors_pulse_guide_start(1, offset, duration_ms);
        break;
    }
    default:
        return mount_result_error("Invalid guide direction");
    }

    return motors_result_code_error_result(rc);
}

/* ── Guide rate storage (deg/s) — per axis ── */

static float s_guide_rate_ra  = TRACKING_SPEED_SIDEREAL_DPS * 0.5f;
static float s_guide_rate_dec = TRACKING_SPEED_SIDEREAL_DPS * 0.5f;

bool mount_guide_rate_is_valid(float rate_dps) {
    return isfinite(rate_dps)
           && rate_dps >= 0.0f
           && rate_dps <= MOUNT_MAX_GUIDE_RATE_DPS;
}

bool mount_set_guide_rate_ra(float rate_dps) {
    if (!mount_guide_rate_is_valid(rate_dps)) {
        return false;
    }
    s_guide_rate_ra = rate_dps;
    return true;
}

bool mount_set_guide_rate_dec(float rate_dps) {
    if (!mount_guide_rate_is_valid(rate_dps)) {
        return false;
    }
    s_guide_rate_dec = rate_dps;
    return true;
}

float mount_get_guide_rate_ra(void)           { return s_guide_rate_ra; }
float mount_get_guide_rate_dec(void)          { return s_guide_rate_dec; }
