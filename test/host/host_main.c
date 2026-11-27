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
#include <stdatomic.h>
#include <time.h>

#include "mount.h"
#include "motors.h"
#include "motors_internal.h"
#include "motion/slew_ramp.h"
#include "host_internal.h"

/* ── Deterministic race hooks ─────────────────────────────────────────────
 * Override the weak motors_test_hook_*() no-ops so tests can pause the producer
 * (MoveAxis) or the motion task at a precise point and observe a STOP landing in
 * that window.
 *
 * The pause/reached flags are written by one thread (the producer, motion task,
 * STOP, or the test) and read by another, so they are C11 atomics — never a
 * plain `volatile int`, which provides neither atomicity nor a happens-before
 * relationship between threads. */
static _Atomic int g_pause_after_enqueue = 0;
static _Atomic int g_reached_enqueue_hook = 0;
static _Atomic int g_pause_before_enqueue = 0;
static _Atomic int g_reached_before_enqueue_hook = 0;
static _Atomic int g_pause_after_process_command = 0;
static _Atomic int g_reached_process_command_hook = 0;
static _Atomic int g_pause_after_stop_begin = 0;
static _Atomic int g_reached_stop_begin_hook = 0;
static _Atomic int g_pause_after_move_axis_stop_open = 0;
static _Atomic int g_reached_move_axis_stop_open_hook = 0;
static _Atomic int g_pause_before_move_axis_tracking_transition = 0;
static _Atomic int g_reached_before_move_axis_tracking_transition_hook = 0;
static _Atomic int g_pause_before_move_axis_tracking_restore = 0;
static _Atomic int g_reached_before_move_axis_tracking_restore_hook = 0;
static _Atomic int g_pause_before_move_axis_saved_tracking_publish = 0;
static _Atomic int g_reached_before_move_axis_saved_tracking_publish_hook = 0;
static _Atomic int g_pause_after_power_reinit = 0;
static _Atomic int g_reached_after_power_reinit = 0;
static _Atomic int g_pause_before_idle_publish = 0;
static _Atomic int g_reached_before_idle_publish = 0;
static _Atomic int g_pause_after_motion_loop = 0;
static _Atomic int g_reached_after_motion_loop = 0;
static _Atomic int g_pause_drain_wait = 0;
static _Atomic int g_reached_drain_wait = 0;
static _Atomic int g_pause_loop_top = 0;
static _Atomic int g_reached_loop_top = 0;

/* Single-shot gate: pause exactly the first producer that reaches the
 * before-enqueue hook while the gate is armed, so a second producer can complete
 * deterministically before the first is released.  Armed by
 * host_arm_first_producer_gate(); the first producer signals reached and then
 * blocks until host_release_first_producer(). */
static _Atomic int g_first_producer_gate = 0;
static _Atomic int g_first_producer_reached = 0;
static _Atomic int g_first_producer_release = 0;

static void pause_on_flag(_Atomic int *flag) {
    while (atomic_load(flag)) {
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000 }; /* 1 ms */
        nanosleep(&ts, NULL);
    }
}

void motors_test_hook_after_move_axis_enqueue(void) {
    atomic_store(&g_reached_enqueue_hook, 1);
    pause_on_flag(&g_pause_after_enqueue);
}

void motors_test_hook_before_move_axis_enqueue(void) {
    atomic_store(&g_reached_before_enqueue_hook, 1);
    pause_on_flag(&g_pause_before_enqueue);

    /* Single-shot gate: only the first producer is paused, so a second producer
     * can run to completion while the first remains suspended.  A spins until the
     * release flag is set (opposite of pause_on_flag's "pause while set"). */
    if (atomic_exchange(&g_first_producer_gate, 0) == 1) {
        atomic_store(&g_first_producer_reached, 1);
        while (atomic_load(&g_first_producer_release) == 0) {
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000 };
            nanosleep(&ts, NULL);
        }
    }
}

void motors_test_hook_after_process_command(void) {
    atomic_store(&g_reached_process_command_hook, 1);
    pause_on_flag(&g_pause_after_process_command);
}

void motors_test_hook_after_stop_begin(void) {
    atomic_store(&g_reached_stop_begin_hook, 1);
    pause_on_flag(&g_pause_after_stop_begin);
}

void motors_test_hook_after_move_axis_stop_open(void) {
    atomic_store(&g_reached_move_axis_stop_open_hook, 1);
    pause_on_flag(&g_pause_after_move_axis_stop_open);
}

void motors_test_hook_before_move_axis_tracking_transition(void) {
    atomic_store(&g_reached_before_move_axis_tracking_transition_hook, 1);
    pause_on_flag(&g_pause_before_move_axis_tracking_transition);
}

void motors_test_hook_before_move_axis_tracking_restore(void) {
    atomic_store(&g_reached_before_move_axis_tracking_restore_hook, 1);
    pause_on_flag(&g_pause_before_move_axis_tracking_restore);
}

void motors_test_hook_before_move_axis_saved_tracking_publish(void) {
    atomic_store(&g_reached_before_move_axis_saved_tracking_publish_hook, 1);
    pause_on_flag(&g_pause_before_move_axis_saved_tracking_publish);
}

void motors_test_hook_after_power_reinit(void) {
    atomic_store(&g_reached_after_power_reinit, 1);
    pause_on_flag(&g_pause_after_power_reinit);
}

void motors_test_hook_before_idle_publish(void) {
    atomic_store(&g_reached_before_idle_publish, 1);
    pause_on_flag(&g_pause_before_idle_publish);
}

void motors_test_hook_after_motion_loop(void) {
    atomic_store(&g_reached_after_motion_loop, 1);
    pause_on_flag(&g_pause_after_motion_loop);
}

void motors_test_hook_drain_wait(void) {
    atomic_store(&g_reached_drain_wait, 1);
    pause_on_flag(&g_pause_drain_wait);
}

void motors_test_hook_loop_top(void) {
    atomic_store(&g_reached_loop_top, 1);
    pause_on_flag(&g_pause_loop_top);
}

void host_set_pause_after_enqueue(int pause) { atomic_store(&g_pause_after_enqueue, pause); }
int host_reached_enqueue_hook(void) { return atomic_load(&g_reached_enqueue_hook); }
void host_clear_enqueue_hook(void) { atomic_store(&g_reached_enqueue_hook, 0); }

void host_set_pause_before_enqueue(int pause) { atomic_store(&g_pause_before_enqueue, pause); }
int host_reached_before_enqueue_hook(void) { return atomic_load(&g_reached_before_enqueue_hook); }
void host_clear_before_enqueue_hook(void) { atomic_store(&g_reached_before_enqueue_hook, 0); }

void host_set_pause_after_process_command(int pause) { atomic_store(&g_pause_after_process_command, pause); }
int host_reached_process_command_hook(void) { return atomic_load(&g_reached_process_command_hook); }
void host_clear_process_command_hook(void) { atomic_store(&g_reached_process_command_hook, 0); }

void host_set_pause_after_stop_begin(int pause) { atomic_store(&g_pause_after_stop_begin, pause); }
int host_reached_stop_begin_hook(void) { return atomic_load(&g_reached_stop_begin_hook); }
void host_clear_stop_begin_hook(void) { atomic_store(&g_reached_stop_begin_hook, 0); }

void host_set_pause_after_move_axis_stop_open(int pause) { atomic_store(&g_pause_after_move_axis_stop_open, pause); }
int host_reached_move_axis_stop_open_hook(void) { return atomic_load(&g_reached_move_axis_stop_open_hook); }
void host_clear_move_axis_stop_open_hook(void) { atomic_store(&g_reached_move_axis_stop_open_hook, 0); }

void host_set_pause_before_move_axis_tracking_transition(int pause) { atomic_store(&g_pause_before_move_axis_tracking_transition, pause); }
int host_reached_before_move_axis_tracking_transition_hook(void) { return atomic_load(&g_reached_before_move_axis_tracking_transition_hook); }
void host_clear_before_move_axis_tracking_transition_hook(void) { atomic_store(&g_reached_before_move_axis_tracking_transition_hook, 0); }

void host_set_pause_before_move_axis_tracking_restore(int pause) { atomic_store(&g_pause_before_move_axis_tracking_restore, pause); }
int host_reached_before_move_axis_tracking_restore_hook(void) { return atomic_load(&g_reached_before_move_axis_tracking_restore_hook); }
void host_clear_before_move_axis_tracking_restore_hook(void) { atomic_store(&g_reached_before_move_axis_tracking_restore_hook, 0); }

void host_set_pause_before_move_axis_saved_tracking_publish(int pause) { atomic_store(&g_pause_before_move_axis_saved_tracking_publish, pause); }
int host_reached_before_move_axis_saved_tracking_publish_hook(void) { return atomic_load(&g_reached_before_move_axis_saved_tracking_publish_hook); }
void host_clear_before_move_axis_saved_tracking_publish_hook(void) { atomic_store(&g_reached_before_move_axis_saved_tracking_publish_hook, 0); }

void host_set_pause_after_power_reinit(int pause) { atomic_store(&g_pause_after_power_reinit, pause); }
int host_reached_after_power_reinit(void) { return atomic_load(&g_reached_after_power_reinit); }
void host_clear_after_power_reinit(void) { atomic_store(&g_reached_after_power_reinit, 0); }

void host_set_pause_before_idle_publish(int pause) { atomic_store(&g_pause_before_idle_publish, pause); }
int host_reached_before_idle_publish(void) { return atomic_load(&g_reached_before_idle_publish); }
void host_clear_before_idle_publish(void) { atomic_store(&g_reached_before_idle_publish, 0); }

void host_set_pause_after_motion_loop(int pause) { atomic_store(&g_pause_after_motion_loop, pause); }
int host_reached_after_motion_loop(void) { return atomic_load(&g_reached_after_motion_loop); }
void host_clear_after_motion_loop(void) { atomic_store(&g_reached_after_motion_loop, 0); }

void host_set_pause_drain_wait(int pause) { atomic_store(&g_pause_drain_wait, pause); }
int host_reached_drain_wait(void) { return atomic_load(&g_reached_drain_wait); }
void host_clear_drain_wait(void) { atomic_store(&g_reached_drain_wait, 0); }

void host_set_pause_loop_top(int pause) { atomic_store(&g_pause_loop_top, pause); }
int host_reached_loop_top(void) { return atomic_load(&g_reached_loop_top); }
void host_clear_loop_top(void) { atomic_store(&g_reached_loop_top, 0); }

void host_arm_first_producer_gate(int arm) { atomic_store(&g_first_producer_gate, arm); }
int host_reached_first_producer_gate(void) { return atomic_load(&g_first_producer_reached); }
void host_clear_first_producer_gate(void) { atomic_store(&g_first_producer_reached, 0); }
void host_release_first_producer(void) { atomic_store(&g_first_producer_release, 1); }


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
                        double unix_seconds, PierSideRequirement required,
                        float speed_dps, AxisCoordinates *out) {
    (void) eq; (void) current; (void) unix_seconds; (void) required;
    (void) speed_dps; (void) out;
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

    motors_control_init();
    motors_hw_init();
    motors_queue_init();
    motors_motion_task_init();
    motors_motion_wait_idle(0, NULL);
}

void host_reset(void) {
    uint32_t gen = motors_stop_begin();
    motors_queue_clear();
    motors_motion_stop();
    motors_motion_wait_idle(gen, NULL);

    portENTER_CRITICAL(&motors_state_lock);
    motors_state.status = MOTORS_STATUS_READY;
    motors_state.tracking = TRACKING_NONE;
    motors_state.guiding = false;
    motors_state.ra_speed = 0.0f;
    motors_state.dec_speed = 0.0f;
    motors_state.ra_steps = 0;
    motors_state.dec_steps = 0;
    motors_state.guide_truncated = false;
    motors_state.position_uncertain = false;
    motors_state.limits.ra_min  = HOST_RA_MIN;
    motors_state.limits.ra_max  = HOST_RA_MAX;
    motors_state.limits.dec_min = HOST_DEC_MIN;
    motors_state.limits.dec_max = HOST_DEC_MAX;
    portEXIT_CRITICAL(&motors_state_lock);

    /* The MoveAxis setpoint tuple and manual-session state are guarded by
     * motors_admission_lock. */
    portENTER_CRITICAL(&motors_admission_lock);
    motors_move_axis_ra_dps = 0.0f;
    motors_move_axis_dec_dps = 0.0f;
    motors_move_axis_version = 0;
    motors_manual_move = false;
    motors_saved_tracking = TRACKING_NONE;
    portEXIT_CRITICAL(&motors_admission_lock);

    mount_set_guide_rate_ra(TRACKING_SPEED_SIDEREAL_DPS * 0.5f);
    mount_set_guide_rate_dec(TRACKING_SPEED_SIDEREAL_DPS * 0.5f);

    /* A latched hardware fault is cleared here so each test starts clean;
     * production clears it only on reboot. */
    motors_reset_hardware_fault();
    /* The stable drain-result record is test-process-global; clear it so each
     * test starts from a clean confirmation. */
    motors_reset_idle_completion();
    /* Reset the TMC stub's cached resolution/chopper/fault-injection state. */
    host_tmc_reset();

    host_step_clear();
    host_rmt_tx_error_axis = -1;
    host_rmt_absent_completion = -1;
    host_rmt_suppress_axis = -1;
    host_rmt_reset_controlled();

    /* Reopen command admission after the reset. */
    motors_admission_open();
    atomic_store(&g_pause_after_enqueue, 0);
    atomic_store(&g_reached_enqueue_hook, 0);
    atomic_store(&g_pause_before_enqueue, 0);
    atomic_store(&g_reached_before_enqueue_hook, 0);
    atomic_store(&g_pause_after_process_command, 0);
    atomic_store(&g_reached_process_command_hook, 0);
    atomic_store(&g_pause_after_stop_begin, 0);
    atomic_store(&g_reached_stop_begin_hook, 0);
    atomic_store(&g_pause_after_move_axis_stop_open, 0);
    atomic_store(&g_reached_move_axis_stop_open_hook, 0);
    atomic_store(&g_pause_before_move_axis_tracking_transition, 0);
    atomic_store(&g_reached_before_move_axis_tracking_transition_hook, 0);
    atomic_store(&g_pause_before_move_axis_tracking_restore, 0);
    atomic_store(&g_reached_before_move_axis_tracking_restore_hook, 0);
    atomic_store(&g_pause_before_move_axis_saved_tracking_publish, 0);
    atomic_store(&g_reached_before_move_axis_saved_tracking_publish_hook, 0);
    atomic_store(&g_pause_after_power_reinit, 0);
    atomic_store(&g_reached_after_power_reinit, 0);
    atomic_store(&g_pause_before_idle_publish, 0);
    atomic_store(&g_reached_before_idle_publish, 0);
    atomic_store(&g_pause_after_motion_loop, 0);
    atomic_store(&g_reached_after_motion_loop, 0);
    atomic_store(&g_pause_drain_wait, 0);
    atomic_store(&g_reached_drain_wait, 0);
    atomic_store(&g_pause_loop_top, 0);
    atomic_store(&g_reached_loop_top, 0);
    atomic_store(&g_first_producer_gate, 0);
    atomic_store(&g_first_producer_reached, 0);
    atomic_store(&g_first_producer_release, 0);
}

void host_wait_idle(void) {
    (void) motors_motion_wait_idle(motors_stop_generation, NULL);
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

int host_mount_park(void) {
    return mount_park().ok ? 1 : 0;
}

int host_mount_unpark(void) {
    return mount_unpark().ok ? 1 : 0;
}

int host_set_guide_rate_ra(float rate) {
    return mount_set_guide_rate_ra(rate) ? 1 : 0;
}

int host_set_guide_rate_dec(float rate) {
    return mount_set_guide_rate_dec(rate) ? 1 : 0;
}

float host_get_guide_rate_ra(void) { return mount_get_guide_rate_ra(); }
float host_get_guide_rate_dec(void) { return mount_get_guide_rate_dec(); }

/* Pure slew-duration estimator — exposed to validate against the oracle. */
float host_slew_duration(float distance_deg, float speed_dps) {
    return slew_ramp_duration(&mount_profile.slew, distance_deg, speed_dps);
}

/* Mechanical scale — read from the compiled profile so the contract test can
 * check it against the declared reduction (rather than trusting it blindly). */
float host_deg_per_step(void) {
    return motors_get_deg_per_microstep();
}

/* Speed ceilings — read from the compiled profile so the contract test can
 * check the per-mount max speeds against the declared values. */
float host_max_slew_dps(void) { return mount_profile.max_slew_dps; }
float host_max_manual_dps(void) { return mount_profile.max_manual_dps; }

/* Per-axis mechanical envelope — asymmetric for BFT DEC [-135, 50]. */
float host_hard_min_deg(int axis) {
    if (axis != AXIS_RA && axis != AXIS_DEC) return 0.0f;
    return mount_profile.axes[axis].hard_min_deg;
}
float host_hard_max_deg(int axis) {
    if (axis != AXIS_RA && axis != AXIS_DEC) return 0.0f;
    return mount_profile.axes[axis].hard_max_deg;
}

/* DIR polarity for an axis (1 = positive rotation is HIGH), from the profile. */
int host_dir_positive_level(int axis) {
    if (axis != AXIS_RA && axis != AXIS_DEC) {
        return -1;
    }
    return mount_profile.axes[axis].dir_positive_level ? 1 : 0;
}

/* Ramp profile for the selected profile, so the duration oracle can be built
 * against the real ramp instead of a hardcoded NRM window. */
float host_slew_accel_deg(void) { return mount_profile.slew.accel_distance_deg; }
float host_slew_decel_deg(void) { return mount_profile.slew.decel_distance_deg; }
float host_slew_min_speed_dps(void) { return mount_profile.slew.min_speed_dps; }
float host_slew_short_distance_deg(void) { return mount_profile.slew.short_distance_deg; }
float host_slew_gentle_distance_deg(void) { return mount_profile.slew.gentle_distance_deg; }
float host_slew_gentle_max_dps(void) { return mount_profile.slew.gentle_max_dps; }

/* Flat snapshot of the compiled mount profile, so the profile-invariant tests
 * read every field they check through one ctypes Structure (no nested structs,
 * bools or enums on the Python side). */
typedef struct {
    const char *id;
    const char *name;
    int geometry;               /* GEOMETRY_GERMAN(0) / GEOMETRY_FORK(1) */
    float ra_reference_offset_deg;
    float full_step_deg;
    float gear_ratio;
    int canonical_microsteps;
    int slew_microsteps;
    float max_slew_dps;
    float max_manual_dps;
    float slew_min_speed_dps;
    float slew_short_distance_deg;
    float slew_gentle_distance_deg;
    float slew_gentle_max_dps;
    float slew_accel_distance_deg;
    float slew_decel_distance_deg;
    float ra_hard_min_deg, ra_hard_max_deg;
    float ra_default_min_deg, ra_default_max_deg;
    float dec_hard_min_deg, dec_hard_max_deg;
    float dec_default_min_deg, dec_default_max_deg;
    int irun, ihold;
    int ra_step_gpio, ra_dir_gpio, ra_uart_num, ra_uart_tx_gpio, ra_uart_rx_gpio;
    int dec_step_gpio, dec_dir_gpio, dec_uart_num, dec_uart_tx_gpio, dec_uart_rx_gpio;
    int led_gpio, buzzer_gpio;
} ProfileSnapshot;

const ProfileSnapshot *host_profile_snapshot(void) {
    static ProfileSnapshot s = {0};
    const MountProfile *p = &mount_profile;
    s.id = p->id;
    s.name = p->name;
    s.geometry = (int) p->geometry;
    s.ra_reference_offset_deg = p->ra_reference_offset_deg;
    s.full_step_deg = p->full_step_deg;
    s.gear_ratio = p->gear_ratio;
    s.canonical_microsteps = p->canonical_microsteps;
    s.slew_microsteps = p->slew_microsteps;
    s.max_slew_dps = p->max_slew_dps;
    s.max_manual_dps = p->max_manual_dps;
    s.slew_min_speed_dps = p->slew.min_speed_dps;
    s.slew_short_distance_deg = p->slew.short_distance_deg;
    s.slew_gentle_distance_deg = p->slew.gentle_distance_deg;
    s.slew_gentle_max_dps = p->slew.gentle_max_dps;
    s.slew_accel_distance_deg = p->slew.accel_distance_deg;
    s.slew_decel_distance_deg = p->slew.decel_distance_deg;
    s.ra_hard_min_deg = p->axes[AXIS_RA].hard_min_deg;
    s.ra_hard_max_deg = p->axes[AXIS_RA].hard_max_deg;
    s.ra_default_min_deg = p->axes[AXIS_RA].default_min_deg;
    s.ra_default_max_deg = p->axes[AXIS_RA].default_max_deg;
    s.dec_hard_min_deg = p->axes[AXIS_DEC].hard_min_deg;
    s.dec_hard_max_deg = p->axes[AXIS_DEC].hard_max_deg;
    s.dec_default_min_deg = p->axes[AXIS_DEC].default_min_deg;
    s.dec_default_max_deg = p->axes[AXIS_DEC].default_max_deg;
    s.irun = p->hardware.irun;
    s.ihold = p->hardware.ihold;
    s.ra_step_gpio = p->hardware.axes[AXIS_RA].step_gpio;
    s.ra_dir_gpio = p->hardware.axes[AXIS_RA].dir_gpio;
    s.ra_uart_num = p->hardware.axes[AXIS_RA].uart_num;
    s.ra_uart_tx_gpio = p->hardware.axes[AXIS_RA].uart_tx_gpio;
    s.ra_uart_rx_gpio = p->hardware.axes[AXIS_RA].uart_rx_gpio;
    s.dec_step_gpio = p->hardware.axes[AXIS_DEC].step_gpio;
    s.dec_dir_gpio = p->hardware.axes[AXIS_DEC].dir_gpio;
    s.dec_uart_num = p->hardware.axes[AXIS_DEC].uart_num;
    s.dec_uart_tx_gpio = p->hardware.axes[AXIS_DEC].uart_tx_gpio;
    s.dec_uart_rx_gpio = p->hardware.axes[AXIS_DEC].uart_rx_gpio;
    s.led_gpio = p->hardware.led_gpio;
    s.buzzer_gpio = p->hardware.buzzer_gpio;
    return &s;
}

/* State accessors. */
int host_status(void) { return (int) motors_current_state().status; }
int host_tracking(void) { return (int) motors_current_state().tracking; }
int host_guiding(void) { return motors_current_state().guiding ? 1 : 0; }
int host_guide_truncated(void) { return motors_current_state().guide_truncated ? 1 : 0; }
int host_position_uncertain(void) { return motors_current_state().position_uncertain ? 1 : 0; }
int host_has_hardware_fault(void) { return motors_has_hardware_fault() ? 1 : 0; }

/* Manual-session observability — read the session globals under their own lock
 * (motors_admission_lock), matching the production writers/readers. */
int host_manual_move(void) {
    bool v;
    portENTER_CRITICAL(&motors_admission_lock);
    v = motors_manual_move;
    portEXIT_CRITICAL(&motors_admission_lock);
    return v ? 1 : 0;
}
int host_saved_tracking(void) {
    TrackingMode v;
    portENTER_CRITICAL(&motors_admission_lock);
    v = motors_saved_tracking;
    portEXIT_CRITICAL(&motors_admission_lock);
    return (int) v;
}

/* MoveAxis setpoint + version — read under the admission lock, matching the
 * producer's publication (motors_queue_put_move_axis) and the public
 * motors_get_move_axis_setpoint. */
float host_move_axis_ra_dps(void) {
    float ra, dec;
    motors_get_move_axis_setpoint(&ra, &dec);
    return ra;
}
float host_move_axis_dec_dps(void) {
    float ra, dec;
    motors_get_move_axis_setpoint(&ra, &dec);
    return dec;
}
uint32_t host_move_axis_version(void) {
    uint32_t ver;
    portENTER_CRITICAL(&motors_admission_lock);
    ver = motors_move_axis_version;
    portEXIT_CRITICAL(&motors_admission_lock);
    return ver;
}

/* Manual-session identity — read under the admission lock, matching the
 * production writers (motors_queue_put_move_axis). */
uint32_t host_manual_session(void) {
    uint32_t v;
    portENTER_CRITICAL(&motors_admission_lock);
    v = motors_manual_session;
    portEXIT_CRITICAL(&motors_admission_lock);
    return v;
}

/* Last published manual-session completion — see motors_finished_manual_*(). */
uint32_t host_finished_manual_session(void) { return motors_finished_manual_session(); }
int host_finished_manual_aborted(void) { return motors_finished_manual_aborted() ? 1 : 0; }

/* Stop-generation observability — a rejected MoveAxis must not advance it. */
uint32_t host_stop_generation(void) { return motors_stop_generation_get(); }

/* Direct admission-gate observability (s_stopping), read under the admission
 * lock — a full queue must not be mistaken for a closed gate. */
int host_admission_closed(void) { return motors_admission_is_closed() ? 1 : 0; }

/* Global stop-confirmation observability. */
uint32_t host_idle_generation(void) { return motors_idle_generation(); }
int host_idle_aborted(void) { return motors_idle_aborted() ? 1 : 0; }
uint32_t host_idle_aborted_generation(void) { return motors_idle_aborted_generation(); }

/* Fill the motion command queue with `n` SLEW commands via the real
 * motors_queue_put, so a subsequent MOVE_AXIS is rejected by a full queue.
 * SLEW is not drained inline by the tracking loop, so the commands stay queued
 * deterministically without disturbing the active motion.  Returns the number
 * of commands actually enqueued. */
int host_fill_queue_with_slew(int n) {
    int enqueued = 0;
    for (int i = 0; i < n; i++) {
        MotionCommand cmd = {
            .type = MOTION_CMD_SLEW,
            .ra_target_deg = 0.0f,
            .dec_target_deg = 0.0f,
            .ra_speed = 0.1f,
            .dec_speed = 0.0f,
            .tracking_mode = TRACKING_NONE,
            .relative = false,
        };
        if (!motors_queue_put(&cmd)) {
            break;
        }
        enqueued++;
    }
    return enqueued;
}

/* Enqueue a real (absolute) SLEW via the production admission path, so a test
 * can drive a coarse/fine slew and check the canonical accounting against the
 * independently-observed pulses.  Returns 1 on success, 0 on a full queue or
 * closed admission. */
int host_enqueue_slew(float ra_target_deg, float dec_target_deg, float speed_dps) {
    MotionCommand cmd = {
        .type = MOTION_CMD_SLEW,
        .ra_target_deg = ra_target_deg,
        .dec_target_deg = dec_target_deg,
        .ra_speed = speed_dps,
        .dec_speed = speed_dps,
        .tracking_mode = TRACKING_NONE,
        .relative = false,
    };
    return motors_queue_put(&cmd) ? 1 : 0;
}

/* Real slew producer — drives the production admission path (validation,
 * tracking pause/resume, enqueue), not a raw enqueue. */
int host_slew_to_angle(float ra_deg, float dec_deg, int speed_rate) {
    return motors_slew_to_angle(ra_deg, dec_deg, speed_rate) == MOTOR_OK ? 1 : 0;
}

/* Active microstep resolution (via the motors/TMC layer) — lets a test assert
 * the physical resolution the drivers report, independent of the counter math. */
int host_microsteps_active(void) { return (int) motors_microsteps_active(); }

/* Peek the type of the command at the front of the motion queue (without
 * removing it), or -1 if the queue is empty.  Lets a test assert that no stale
 * command (e.g. a tracking restore) was left queued. */
int host_queue_front_type(void) {
    if (motion_cmd_queue == NULL) {
        return -1;
    }
    MotionCommand queued;
    if (xQueuePeek(motion_cmd_queue, &queued, 0) == pdTRUE) {
        return (int) queued.type;
    }
    return -1;
}

void host_update_power(int power_ok) { motors_update_power(power_ok ? true : false); }

/* Latch a hardware fault through the real production entry point, so a test can
 * exercise a fault concurrent with the power recovery (the RMT tx-failure path
 * requires a running motion, which ERROR rejects). */
int host_enter_hardware_fault(void) {
    motors_enter_hardware_fault();
    return 1;
}
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
