/* Host harness — drives the production mount/motors code (scheduler, queue,
 * motion loop, RMT encoding) with RTOS/RMT/time mocked, and exposes a small C
 * API the Python tests call through ctypes.
 *
 * The production motion task runs in its own pthread (motors_motion_task_init);
 * the RMT mock records every STEP pulse so tests can observe the actual
 * transmissions rather than trusting the firmware's position counters.
 */
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#include "mount.h"
#include "motors.h"
#include "motors_internal.h"
#include "host_internal.h"

/* ── Module globals normally defined by motors_init.c / mount_init.c ── */

MotorsState motors_state = {
    .ra_steps = 0,
    .dec_steps = 0,
    .status = MOTORS_STATUS_READY,
    .tracking = TRACKING_NONE,
    .ra_speed = 0.0f,
    .dec_speed = 0.0f,
    .guiding = false,
};

portMUX_TYPE motors_state_lock = portMUX_INITIALIZER_UNLOCKED;

MountSettings mount_internal_state;

/* ── Coordinate helpers (only referenced by mount_tools.c's unused accessors;
 *    the guide path uses motors_get_*_deg directly, not these). ── */

EquatorialCoordinates axis_to_equatorial(AxisCoordinates axis) {
    (void) axis;
    return (EquatorialCoordinates){0};
}

bool equatorial_to_axis(EquatorialCoordinates eq, AxisCoordinates current,
                        time_t at_time, AxisCoordinates *out) {
    (void) eq; (void) current; (void) at_time; (void) out;
    return false;
}

/* ── Harness API ── */

static const float HOST_RA_MIN  = -100.0f;
static const float HOST_RA_MAX  = 100.0f;
static const float HOST_DEC_MIN = -150.0f;
static const float HOST_DEC_MAX = 150.0f;

static bool s_host_initialized = false;

void host_init(void) {
    /* Idempotent: each test class constructs its own Harness, which calls
     * host_init again.  The production motion task, command queue and RMT
     * channels are process-global — a second init would leak the first task
     * (a dangling thread blocked on a stale queue) and corrupt the step counts
     * the baseline test measures.  Initialise once, reuse for every class. */
    if (s_host_initialized) {
        return;
    }
    s_host_initialized = true;

    portENTER_CRITICAL(&motors_state_lock);
    motors_state.limits.ra_min  = HOST_RA_MIN;
    motors_state.limits.ra_max  = HOST_RA_MAX;
    motors_state.limits.dec_min = HOST_DEC_MIN;
    motors_state.limits.dec_max = HOST_DEC_MAX;
    portEXIT_CRITICAL(&motors_state_lock);

    motors_hw_init();
    motors_queue_init();
    motors_motion_task_init();
    motors_motion_wait_idle();
}

void host_reset(void) {
    motors_motion_stop();
    motors_queue_clear();
    motors_motion_wait_idle();

    portENTER_CRITICAL(&motors_state_lock);
    motors_state.status = MOTORS_STATUS_READY;
    motors_state.tracking = TRACKING_NONE;
    motors_state.guiding = false;
    motors_state.ra_speed = 0.0f;
    motors_state.dec_speed = 0.0f;
    motors_state.ra_steps = 0;
    motors_state.dec_steps = 0;
    motors_state.guide_truncated = false;
    motors_state.limits.ra_min  = HOST_RA_MIN;
    motors_state.limits.ra_max  = HOST_RA_MAX;
    motors_state.limits.dec_min = HOST_DEC_MIN;
    motors_state.limits.dec_max = HOST_DEC_MAX;
    portEXIT_CRITICAL(&motors_state_lock);

    mount_move_axis_reset();
    mount_set_guide_rate_ra(TRACKING_SPEED_SIDEREAL_DPS * 0.5f);
    mount_set_guide_rate_dec(TRACKING_SPEED_SIDEREAL_DPS * 0.5f);

    /* A latched hardware fault is cleared here so each test starts clean;
     * production clears it only on reboot. */
    motors_reset_hardware_fault();

    host_step_clear();
    host_rmt_tx_error_axis = -1;
    host_rmt_absent_completion = -1;
    host_rmt_suppress_axis = -1;
}

void host_wait_idle(void) {
    motors_motion_wait_idle();
}

/* Mount commands — return 1 on ok, 0 on rejection. */
int host_mount_guide(int direction, uint32_t duration_ms) {
    return mount_pulse_guide((GuideDirection) direction, duration_ms).ok ? 1 : 0;
}

int host_mount_tracking(int mode) {
    return mount_set_tracking((TrackingMode) mode).ok ? 1 : 0;
}

int host_mount_move_axis_speed(float ra, float dec) {
    return mount_set_move_axis_speed(ra, dec).ok ? 1 : 0;
}

int host_mount_move_axis_rate(int axis, float rate) {
    return mount_set_move_axis_rate(axis, rate).ok ? 1 : 0;
}

int host_mount_stop(void) {
    return mount_stop().ok ? 1 : 0;
}

int host_set_guide_rate_ra(float rate) {
    return mount_set_guide_rate_ra(rate) ? 1 : 0;
}

int host_set_guide_rate_dec(float rate) {
    return mount_set_guide_rate_dec(rate) ? 1 : 0;
}

float host_get_guide_rate_ra(void) { return mount_get_guide_rate_ra(); }
float host_get_guide_rate_dec(void) { return mount_get_guide_rate_dec(); }

/* State accessors. */
int host_status(void) { return (int) motors_current_state().status; }
int host_tracking(void) { return (int) motors_current_state().tracking; }
int host_guiding(void) { return motors_current_state().guiding ? 1 : 0; }
int host_guide_truncated(void) { return motors_current_state().guide_truncated ? 1 : 0; }
int host_has_hardware_fault(void) { return motors_has_hardware_fault() ? 1 : 0; }
void host_update_power(int power_ok) { motors_update_power(power_ok ? true : false); }
int64_t host_ra_steps(void) { return motors_current_state().ra_steps; }
int64_t host_dec_steps(void) { return motors_current_state().dec_steps; }

void host_set_ra_deg(float deg) {
    portENTER_CRITICAL(&motors_state_lock);
    motors_state.ra_steps = motors_deg_to_steps(deg);
    portEXIT_CRITICAL(&motors_state_lock);
}

void host_set_dec_deg(float deg) {
    portENTER_CRITICAL(&motors_state_lock);
    motors_state.dec_steps = motors_deg_to_steps(deg);
    portEXIT_CRITICAL(&motors_state_lock);
}

/* RMT fault injection. */
void host_rmt_set_tx_error(int axis)       { host_rmt_tx_error_axis = axis; }
void host_rmt_set_absent(int axis)         { host_rmt_absent_completion = axis; }
void host_rmt_set_suppress(int axis)       { host_rmt_suppress_axis = axis; }
void host_rmt_clear_faults(void) {
    host_rmt_tx_error_axis = -1;
    host_rmt_absent_completion = -1;
    host_rmt_suppress_axis = -1;
}
