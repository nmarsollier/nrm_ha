/* Motors - motors_motion_task.c
 *
 * The motion task consumes MotionCommands from the queue and drives axis
 * positions via RMT+DMA step pulses.  It is the sole writer of motors_state
 * position fields during slews and tracking.
 *
 * Two execution paths, dispatched by command type:
 *   slewing_loop_rmt  — distance-bounded, ramped accel/decel, batched RMT
 *   tracking_loop_rmt — open-ended, constant velocity, fractional accumulator
 */

#include "motors_internal.h"

#include "motion/slew_ramp.h"

#include <math.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "MOTORS_MOTION_TASK";

/* Test hooks — weak no-ops overridden by the host harness to pause the executor
 * at a precise point so a STOP/fault can land deterministically in that window. */
void __attribute__((weak)) motors_test_hook_after_process_command(void) {}
void __attribute__((weak)) motors_test_hook_before_move_axis_saved_tracking_publish(void) {}
void __attribute__((weak)) motors_test_hook_before_idle_publish(void) {}
void __attribute__((weak)) motors_test_hook_drain_wait(void) {}
void __attribute__((weak)) motors_test_hook_loop_top(void) {}
void __attribute__((weak)) motors_test_hook_after_motion_loop(void) {}

/* =========================================================================
 * Definitions and state
 * ========================================================================= */

#define MOTION_TASK_STACK_WORDS 6144  /* 24 KB — room for 2×256-symbol ping-pong buffers */
#define MOTION_TASK_PRIORITY    23  /* near real-time */

/* Step period ceiling in RMT ticks — used when velocity is zero or unknown. */
#define MAX_STEP_PERIOD_TICKS (20U * 1000U * 1000U) /* 10 s at 2 MHz */

/* Target batch duration in RMT ticks (≈20 ms at 2 MHz): shorter batches give
 * finer ramp granularity at higher CPU cost. */
#define RMT_BATCH_TARGET_TICKS  320000U

/* 256 symbols (1 KB) fits one GDMA descriptor; a batch is at most 100 steps.
 * RMT_BATCH_MAX_STEPS also bounds the uncounted error of an aborted motion
 * (≈33.75″) — accepted, since STOP during a slew is rare and stays bounded. */
#define RMT_BUFFER_SYMBOLS  256U
#define RMT_BATCH_MAX_STEPS  100U

/* Stop/drain timeout.  Worst in-flight work is two batches (ping+pong) per axis,
 * ≤100 pulses each; at the slowest coarse slew one pulse is ~0.94 ms, so
 * 2×100 pulses ≈ 190 ms.  500 ms is a ~2.6× margin. */
#define MOTION_DRAIN_TIMEOUT_US  500000

/* Idle confirmation must outlast the drain deadline plus loop-exit teardown and
 * the idle re-publish latency. */
#define MOTION_IDLE_TIMEOUT_US  1000000

/* Idle queue poll period: keeps a STOP that lands while idle from sleeping past
 * the rest confirmation. */
#define MOTION_IDLE_QUEUE_POLL_MS 100

/* Single-step wait during tracking: 500 ms is a pure hang-detection bound (a
 * pulse completes in µs or never).  Deliberately ≤ MOTION_IDLE_TIMEOUT_US so a
 * STOP's rest confirmation always outlasts it. */
#define TRACKING_STEP_TIMEOUT_US 500000
#define TRACKING_STEP_SLICE_MS   50

TaskHandle_t motors_motion_task_handle = NULL;

/* Stop epoch — bumped on every stop (atomically with admission) so commands
 * enqueued before the stop are discarded by the motion task. */
volatile uint32_t motors_stop_generation = 0;

/* True while the motion task is blocked on the command queue, false while it
 * runs a motion loop. */
static volatile bool s_motion_idle = true;

/* Rest confirmation published by the motion task under motors_state_lock:
 *   s_idle_generation          — generation at which the executor is now at
 *                                rest (re-published every barrier).  Monotonic.
 *   s_idle_aborted_generation  — generation the drain result below belongs to,
 *                                published only when a motion actually ends. */
static volatile uint32_t s_idle_generation = 0;
static volatile bool s_idle_aborted = false;
static volatile uint32_t s_idle_aborted_generation = 0;

/* Explicit manual-session completion, published by the executor (single writer).
 * The finalization waits on s_finished_manual_session >= session, so an
 * unrelated idle barrier or setpoint-version preemption can never confirm it. */
static volatile uint32_t s_finished_manual_session = 0;
static volatile bool s_finished_manual_aborted = false;

/* Active command state.  `active` and `graceful` are written by
 * motors_motion_stop() from any task; every other field is single-writer
 * (motion task only). */
static struct {
    volatile bool active;      /* false → abort at next iteration (any task) */
    bool aborted;              /* true → loop exited via fault/preempt, not completion */
    volatile bool graceful;    /* true → STOP requested; drain in-flight batches (any task) */
    bool completed;            /* true → READY must be published at the idle barrier */
    bool ran;                  /* true → a motion actually ran this iteration */
    /* Drain identity, frozen at the end of motion_loop (not at the barrier), so
     * a later preemption or STOP cannot re-tag the published result. */
    uint32_t drain_generation;
    bool drain_graceful;
    bool drain_aborted;
    /* True when the current MoveAxis motion was preempted by a newer setpoint
     * in the same session: drains the committed batches without finalising the
     * session (single-writer, motion task). */
    bool preempted;
    volatile MotionCommandType active_cmd_type;  /* volatile: read by the manual-drain wait */
    int64_t ra_target;         /* steps */
    int64_t dec_target;        /* steps */
    int64_t ra_start;          /* steps — captured at motion start (for ramps) */
    int64_t dec_start;         /* steps */
    TrackingMode resume_tracking;   /* restore after the current slew (SLEW only) */
    uint32_t move_axis_version;     /* setpoint version captured at motion start */
    uint32_t move_axis_session;     /* manual-session identity of the active move */

    /* PulseGuide state (single-writer, motion task). */
    int64_t ra_guide_deadline_us;
    int64_t dec_guide_deadline_us;
    float ra_guide_offset_dps;
    float dec_guide_offset_dps;
} s_motion;

/* =========================================================================
 * Common helpers
 * ========================================================================= */

/* Clear any in-flight guide pulse so a stale deadline/offset cannot "resume"
 * the pulse after a stop or a hardware fault. */
static void clear_guide_state(void) {
    s_motion.ra_guide_deadline_us = 0;
    s_motion.ra_guide_offset_dps = 0.0f;
    s_motion.dec_guide_deadline_us = 0;
    s_motion.dec_guide_offset_dps = 0.0f;
}

/* RMT emission failure — an unrecoverable hardware fault (survives power
 * recovery), not a user STOP or a limit. */
static void abort_rmt_hardware_fault(void) {
    s_motion.active = false;
    s_motion.aborted = true;
    clear_guide_state();
    motors_enter_hardware_fault();
}

/* Mark the motion finished.  READY is published at the idle barrier, not here,
 * so a reader never observes READY while the task is still leaving its loop. */
static void finish_motion(void) {
    s_motion.active = false;
    s_motion.completed = true;
}

/* Returns false when the current motion should end. */
static bool check_motion_conditions(void) {
    if (motors_status_is_error(motors_state.status)) {
        motors_enter_error_state();
        s_motion.active = false;
        s_motion.aborted = true;
        return false;
    }

    int64_t ra_diff = s_motion.ra_target - motors_state.ra_steps;
    if (ra_diff < 0) ra_diff = -ra_diff;
    int64_t dec_diff = s_motion.dec_target - motors_state.dec_steps;
    if (dec_diff < 0) dec_diff = -dec_diff;

    bool ra_has_target = (ra_diff > 0);
    bool dec_has_target = (dec_diff > 0);

    /* Slew/move-axis completion: both axes at target.  Use active_cmd_type (not
     * status), which the resume-after-slew pattern may already have changed. */
    if ((s_motion.active_cmd_type == MOTION_CMD_SLEW ||
         s_motion.active_cmd_type == MOTION_CMD_MOVE_AXIS) &&
        !ra_has_target && !dec_has_target) {
        s_motion.completed = true;
        s_motion.active = false;

        return false;
    }

    /* External tracking stop. */
    if (motors_state.tracking == TRACKING_NONE &&
        motors_state.status == MOTORS_STATUS_TRACKING) {
        s_motion.completed = true;
        s_motion.active = false;

        return false;
    }

    return true;
}

/* =========================================================================
 * Stop the active motion loop (from outside the motion task)
 * ========================================================================= */

void motors_motion_stop(void) {
    /* The stop generation is advanced separately by motors_stop_begin() so that
     * admission-close and generation-bump are one atomic step.  The flags are
     * written under the admission lock — the same mechanism the motion task uses
     * to read them at motion start. */
    portENTER_CRITICAL(&motors_admission_lock);
    motors_motion_mark_stopped_locked();
    portEXIT_CRITICAL(&motors_admission_lock);

    clear_guide_state();

    if (motors_motion_task_handle) {
        /* eIncrement: the tracking loop sleeps in ulTaskNotifyTake(pdTRUE, …),
         * which only wakes early on a non-zero notification count. */
        xTaskNotifyGive(motors_motion_task_handle);
    }
}

/* Mark the running motion as gracefully stopped, called with the admission lock
 * held, so the generation bump and the graceful flag are one coherent critical
 * section. */
void motors_motion_mark_stopped_locked(void) {
    s_motion.active = false;
    s_motion.graceful = true;
}

/* =========================================================================
 * Command processing
 * ========================================================================= */

/* Clamp an absolute step target to the configured RA/DEC limits. */
static int64_t clamp_ra_steps(int64_t steps) {
    int64_t lo = motors_deg_to_steps(motors_state.limits.ra_min);
    int64_t hi = motors_deg_to_steps(motors_state.limits.ra_max);
    if (steps < lo) return lo;
    if (steps > hi) return hi;
    return steps;
}

static int64_t clamp_dec_steps(int64_t steps) {
    int64_t lo = motors_deg_to_steps(motors_state.limits.dec_min);
    int64_t hi = motors_deg_to_steps(motors_state.limits.dec_max);
    if (steps < lo) return lo;
    if (steps > hi) return hi;
    return steps;
}

/* Extend or replace a PulseGuide deadline for one axis: same direction extends,
 * opposite direction replaces. */
static void update_guide(
        int64_t *deadline_us,
        float *offset_dps,
        const MotionCommand *cmd)
{
    int64_t new_deadline = esp_timer_get_time()
                         + (int64_t)cmd->guide_duration_ms * 1000;

    if (*deadline_us && *offset_dps == cmd->guide_offset_dps) {
        if (new_deadline > *deadline_us)
            *deadline_us = new_deadline;
        return;
    }

    *deadline_us = new_deadline;
    *offset_dps = cmd->guide_offset_dps;
}

/* Enter tracking for `mode`: set the base rate and the open-ended RA target so
 * the loop runs until an external stop.  Shared by the TRACK command and the
 * inline tracking resume after a slew. */
static void begin_tracking(TrackingMode mode) {
    float ra_speed = motors_get_tracking_speed(mode);

    portENTER_CRITICAL(&motors_state_lock);
    motors_state.ra_speed = ra_speed;
    motors_state.dec_speed = 0.0f;
    motors_state.status = MOTORS_STATUS_TRACKING;
    motors_state.tracking = mode;
    portEXIT_CRITICAL(&motors_state_lock);

    /* Open-ended: target the axis limit so the loop never completes on its own. */
    s_motion.ra_target = (ra_speed >= 0.0f)
        ? motors_deg_to_steps(motors_state.limits.ra_max)
        : motors_deg_to_steps(motors_state.limits.ra_min);
    s_motion.dec_target = motors_state.dec_steps;
    s_motion.ra_start = motors_state.ra_steps;
    s_motion.dec_start = motors_state.dec_steps;
    s_motion.active_cmd_type = MOTION_CMD_TRACK;
    s_motion.active = true;
}

static void begin_slew(const MotionCommand *cmd) {
    portENTER_CRITICAL(&motors_state_lock);
    /* Capture the tracking mode to restore after the slew: the tracking loop
     * just ended, so the mode read here is the live one. */
    s_motion.resume_tracking = (motors_state.status == MOTORS_STATUS_TRACKING
                                && motors_state.tracking != TRACKING_NONE)
                                   ? motors_state.tracking : TRACKING_NONE;
    motors_state.ra_speed = cmd->ra_speed;
    motors_state.dec_speed = cmd->dec_speed;
    motors_state.status = MOTORS_STATUS_SLEWING;
    motors_state.tracking = TRACKING_NONE;
    motors_state.guiding = false;   /* a slew cancels any in-flight guide */
    portEXIT_CRITICAL(&motors_state_lock);

    clear_guide_state();

    if (cmd->relative) {
        s_motion.ra_target = motors_state.ra_steps
                             + motors_deg_to_steps(cmd->ra_delta_deg);
        s_motion.dec_target = motors_state.dec_steps
                              + motors_deg_to_steps(cmd->dec_delta_deg);
    } else {
        s_motion.ra_target = motors_deg_to_steps(cmd->ra_target_deg);
        s_motion.dec_target = motors_deg_to_steps(cmd->dec_target_deg);
    }
    /* Re-clamp at execution: the relative destination is computed from the live
     * position, which may have advanced since the request was validated. */
    s_motion.ra_target = clamp_ra_steps(s_motion.ra_target);
    s_motion.dec_target = clamp_dec_steps(s_motion.dec_target);
    s_motion.ra_start = motors_state.ra_steps;
    s_motion.dec_start = motors_state.dec_steps;
    s_motion.active = true;
}

static void begin_move_axis(const MotionCommand *cmd) {
    /* MoveAxis is a setpoint: read the newest published rate (guarded by the
     * admission lock), not the possibly-stale queued value. */
    float ra_sp, dec_sp;
    uint32_t ver;

    /* Discard a MoveAxis whose manual session no longer matches the active one
     * (finalised or replaced).  A discarded MoveAxis marks its session finished. */
    bool session_active;
    uint32_t cur_session;
    portENTER_CRITICAL(&motors_admission_lock);
    session_active = motors_manual_move;
    cur_session = motors_manual_session;
    portEXIT_CRITICAL(&motors_admission_lock);
    if (!session_active || cmd->session != cur_session) {
        portENTER_CRITICAL(&motors_state_lock);
        if (cmd->session > s_finished_manual_session) {
            s_finished_manual_session = cmd->session;
            s_finished_manual_aborted = false;
        }
        if (!session_active
            && motors_state.status == MOTORS_STATUS_SLEWING) {
            motors_state.status = MOTORS_STATUS_READY;
        }
        portEXIT_CRITICAL(&motors_state_lock);
        return;
    }

    portENTER_CRITICAL(&motors_admission_lock);
    ra_sp = motors_move_axis_ra_dps;
    dec_sp = motors_move_axis_dec_dps;
    ver = motors_move_axis_version;
    portEXIT_CRITICAL(&motors_admission_lock);

    /* Capture the tracking mode to restore when the manual session ends — the
     * tracking loop just ended, so the mode read here is the live one. */
    TrackingMode saved = TRACKING_NONE;
    portENTER_CRITICAL(&motors_state_lock);
    if (motors_state.status == MOTORS_STATUS_TRACKING
        && motors_state.tracking != TRACKING_NONE) {
        saved = motors_state.tracking;
    }
    motors_state.ra_speed = fabsf(ra_sp);
    motors_state.dec_speed = fabsf(dec_sp);
    motors_state.status = MOTORS_STATUS_SLEWING;
    motors_state.tracking = TRACKING_NONE;
    portEXIT_CRITICAL(&motors_state_lock);

    s_motion.ra_target = (ra_sp > 0.0f)
                             ? motors_deg_to_steps(motors_state.limits.ra_max)
                             : (ra_sp < 0.0f)
                                   ? motors_deg_to_steps(motors_state.limits.ra_min)
                                   : motors_state.ra_steps;
    s_motion.dec_target = (dec_sp > 0.0f)
                              ? motors_deg_to_steps(motors_state.limits.dec_max)
                              : (dec_sp < 0.0f)
                                    ? motors_deg_to_steps(motors_state.limits.dec_min)
                                    : motors_state.dec_steps;

    s_motion.ra_start = motors_state.ra_steps;
    s_motion.dec_start = motors_state.dec_steps;
    s_motion.move_axis_version = ver;
    s_motion.move_axis_session = cmd->session;
    s_motion.active = true;

    motors_test_hook_before_move_axis_saved_tracking_publish();

    /* Record the saved tracking only when transitioning from tracking AND the
     * session is still the one being processed.  The check and the publication
     * are one admission-lock critical section. */
    if (saved != TRACKING_NONE) {
        portENTER_CRITICAL(&motors_admission_lock);
        if (motors_manual_move
            && motors_manual_session == cmd->session) {
            motors_saved_tracking = saved;
        }
        portEXIT_CRITICAL(&motors_admission_lock);
    }
}

static void begin_pulse_guide(const MotionCommand *cmd) {
    if (cmd->guide_axis == 0) {
        update_guide(&s_motion.ra_guide_deadline_us,
                     &s_motion.ra_guide_offset_dps, cmd);
    } else {
        update_guide(&s_motion.dec_guide_deadline_us,
                     &s_motion.dec_guide_offset_dps, cmd);
    }

    motors_state.guiding = true;
    motors_state.guide_truncated = false;   /* a fresh guide clears the truncation latch */
    /* Set active so a standalone guide pulse (READY, no tracking) runs its
     * motion loop; during tracking this is a no-op. */
    s_motion.active = true;
}

static void process_command(MotionCommand cmd) {
    /* Reject a command that was queued while the motors were (or are) in ERROR. */
    if (motors_status_is_error(motors_state.status)) {
        return;
    }

    /* Discard a command invalidated by a STOP that landed after it was enqueued.
     * Read the generation under the admission lock — the same mechanism the STOP
     * uses to bump it. */
    if (cmd.generation != motors_stop_generation_get()) {
        return;
    }

    s_motion.active_cmd_type = cmd.type;
    s_motion.resume_tracking = TRACKING_NONE;

    switch (cmd.type) {
        case MOTION_CMD_SLEW:
            begin_slew(&cmd);
            break;
        case MOTION_CMD_TRACK:
            begin_tracking(cmd.tracking_mode);
            break;
        case MOTION_CMD_MOVE_AXIS:
            begin_move_axis(&cmd);
            break;
        case MOTION_CMD_PULSE_GUIDE:
            begin_pulse_guide(&cmd);
            break;
    }
}

/* =========================================================================
 * Slew and MoveAxis — batched RMT execution
 * ========================================================================= */

/* Ramp velocity in deg/s from the executor's step/centidegree state. */
static float slew_velocity_dps(float speed_dps, int64_t travelled_steps,
                               int64_t distance_steps, int distance_cds) {
    float distance_deg = (float) distance_cds / 100.0f;
    float travelled_deg = (distance_steps > 0)
        ? (float) travelled_steps * distance_deg / (float) distance_steps
        : 0.0f;
    return slew_ramp_velocity(&mount_profile.slew, distance_deg, travelled_deg, speed_dps);
}

/* Step period in RMT ticks for an angular speed.  `spp` (steps per pulse) is
 * captured once at motion start, so the period is never re-read from the mutable
 * TMC state mid-motion. */
static uint32_t step_period_ticks(float velocity_dps, uint32_t spp) {
    if (fabsf(velocity_dps) < 1e-9f)
        return MAX_STEP_PERIOD_TICKS;

    float deg_per_pulse = motors_get_deg_per_microstep() * (float) spp;
    float period_s = deg_per_pulse / fabsf(velocity_dps);
    uint32_t ticks = (uint32_t) (period_s * (float) RMT_RESOLUTION_HZ);
    if (ticks < STEP_MIN_PERIOD_TICKS)
        ticks = STEP_MIN_PERIOD_TICKS;
    return ticks;
}

/* Clamp a batch to remaining steps and the hard max-step ceiling. */
static uint32_t compute_batch_size(uint32_t period_ticks, uint32_t remaining) {
    uint32_t batch = (period_ticks < RMT_BATCH_TARGET_TICKS)
                         ? RMT_BATCH_TARGET_TICKS / period_ticks
                         : 1;

    if (batch > remaining)
        batch = remaining;

    if (batch > RMT_BATCH_MAX_STEPS)
        batch = RMT_BATCH_MAX_STEPS;

    return batch;
}

/* Clamp a batch so the axis never emits a step past its limit.  Applied per
 * batch, so a limit changed while a move is in flight still holds. */
static uint32_t clamp_ra_batch(int64_t current, int sign, uint32_t batch, uint32_t spp) {
    int64_t limit = (sign > 0) ? motors_deg_to_steps(motors_state.limits.ra_max)
                               : motors_deg_to_steps(motors_state.limits.ra_min);
    int64_t headroom = (sign > 0) ? (limit - current) : (current - limit);
    uint32_t max_steps = (headroom > 0)
                             ? (uint32_t)(headroom / (int64_t) spp)
                             : 0;
    return (batch > max_steps) ? max_steps : batch;
}

static uint32_t clamp_dec_batch(int64_t current, int sign, uint32_t batch, uint32_t spp) {
    int64_t limit = (sign > 0) ? motors_deg_to_steps(motors_state.limits.dec_max)
                               : motors_deg_to_steps(motors_state.limits.dec_min);
    int64_t headroom = (sign > 0) ? (limit - current) : (current - limit);
    uint32_t max_steps = (headroom > 0)
                             ? (uint32_t)(headroom / (int64_t) spp)
                             : 0;
    return (batch > max_steps) ? max_steps : batch;
}

/* Recompute RA/DEC step-period ticks from the current ramp or constant speed. */
static void compute_slew_periods(
        bool is_move_axis,
        int64_t ra_travelled,
        int64_t dec_travelled,
        int64_t ra_dist,
        int64_t dec_dist,
        int ra_distance_cds,
        int dec_distance_cds,
        uint32_t spp,
        uint32_t *ra_period,
        uint32_t *dec_period)
{
    if (is_move_axis) {
        *ra_period = step_period_ticks(motors_state.ra_speed, spp);
        *dec_period = step_period_ticks(motors_state.dec_speed, spp);
        return;
    }

    float ra_vel = slew_velocity_dps(motors_state.ra_speed,
                                       ra_travelled, ra_dist, ra_distance_cds);
    float dec_vel = slew_velocity_dps(motors_state.dec_speed,
                                      dec_travelled, dec_dist, dec_distance_cds);

    *ra_period = step_period_ticks(ra_vel, spp);
    *dec_period = step_period_ticks(dec_vel, spp);
}

/* Encode one axis batch.  Shared by the initial batch and the pre-encoded
 * batches of the double-buffered slewing loop. */
static uint32_t encode_axis_batch(rmt_symbol_word_t *buf, uint32_t max_sym,
                                   uint32_t batch, uint32_t period_ticks,
                                   uint32_t spp,
                                   float speed_dps,
                                   int64_t base_travelled, int64_t dist_steps,
                                   int distance_cds, bool is_move_axis)
{
    if (batch == 0) return 0;

    if (is_move_axis) {
        return motors_rmt_encode_steps(buf, max_sym, period_ticks, batch);
    }

    uint32_t total_sym = 0;
    rmt_symbol_word_t *sym = buf;
    for (uint32_t i = 0; i < batch && total_sym < max_sym; i++) {
        int64_t travelled = base_travelled + (int64_t)i;
        float vel = slew_velocity_dps(speed_dps, travelled, dist_steps, distance_cds);
        uint32_t period = step_period_ticks(vel, spp);
        uint32_t n = motors_rmt_encode_steps(sym, max_sym - total_sym,
                                              period, 1);
        sym += n;
        total_sym += n;
    }
    return total_sym;
}

/* Correct the coarse-microstep residual after a slew: switch back to fine
 * resolution and emit one pulse per remaining canonical step.  Returns false
 * only on abort/error. */
static bool slew_residual_correction(int64_t ra_dist, int64_t dec_dist,
                                     uint32_t total_ra_steps, uint32_t total_dec_steps,
                                     int ra_sign, int dec_sign, uint32_t spp)
{
    int ra_residual = (int)(ra_dist - (int64_t)total_ra_steps * (int64_t)spp);
    int dec_residual = (int)(dec_dist - (int64_t)total_dec_steps * (int64_t)spp);

    if (ra_residual == 0 && dec_residual == 0) {
        return true;
    }

    if (!motors_microsteps_prepare(MOTION_CMD_TRACK, 0, 0)) {
        return false;
    }

    rmt_symbol_word_t sym;

    for (int i = 0; i < ra_residual; i++) {
        if (!s_motion.active) return false;
        int64_t next = motors_state.ra_steps + (int64_t)ra_sign;
        if (!motors_is_valid_ra_steps(next)) break;

        motors_rmt_encode_pulse(&sym);
        if (motors_rmt_transmit_ra(&sym, 1) != ESP_OK
            || motors_rmt_wait_ra(pdMS_TO_TICKS(TRACKING_STEP_TIMEOUT_US / 1000)) != ESP_OK) {
            abort_rmt_hardware_fault();
            return false;
        }
        motors_state.ra_steps = next;
    }

    for (int i = 0; i < dec_residual; i++) {
        if (!s_motion.active) return false;
        int64_t next = motors_state.dec_steps + (int64_t)dec_sign;
        if (!motors_is_valid_dec_steps(next)) break;

        motors_rmt_encode_pulse(&sym);
        if (motors_rmt_transmit_dec(&sym, 1) != ESP_OK
            || motors_rmt_wait_dec(pdMS_TO_TICKS(TRACKING_STEP_TIMEOUT_US / 1000)) != ESP_OK) {
            abort_rmt_hardware_fault();
            return false;
        }
        motors_state.dec_steps = next;
    }

    return true;
}

/* Account the two submitted batches (ping + pong) for one axis, each exactly
 * once, within a shared deadline.  Returns false when the deadline expires with
 * a batch unconfirmed. */
static bool drain_axis_batches(uint32_t batch[2], int ping, int pong,
                               int sign, uint32_t spp, int64_t *steps,
                               bool (*try_wait)(void), int64_t deadline_us) {
    while (batch[ping] > 0 || batch[pong] > 0) {
        if (batch[ping] == 0 && batch[pong] > 0) {
            batch[ping] = batch[pong];
            batch[pong] = 0;
        }
        if (batch[ping] > 0 && try_wait()) {
            /* Consume and account even past the deadline, so a completed batch
             * is never lost because the *other* axis exhausted it. */
            portENTER_CRITICAL(&motors_state_lock);
            *steps += (int64_t) sign * (int64_t) batch[ping] * (int64_t) spp;
            portEXIT_CRITICAL(&motors_state_lock);
            batch[ping] = batch[pong];
            batch[pong] = 0;
        } else if (batch[ping] > 0) {
            motors_test_hook_drain_wait();
            if (motors_status_is_error(motors_state.status)) {
                return false;
            }
            if (esp_timer_get_time() > deadline_us) {
                return false;
            }
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
    return true;
}

/* Double-buffered slewing / move-axis loop.  Each axis advances independently
 * as its in-flight batch completes, so a slow axis never stalls a fast one. */
static void slewing_loop_rmt(void) {
    int64_t ra_dist = s_motion.ra_target - s_motion.ra_start;
    int64_t dec_dist = s_motion.dec_target - s_motion.dec_start;
    if (ra_dist < 0) ra_dist = -ra_dist;
    if (dec_dist < 0) dec_dist = -dec_dist;

    /* Counts below are in STEP pulses: one pulse advances `spp` canonical steps. */
    uint32_t spp = motors_steps_per_pulse();
    uint32_t total_ra_steps = (uint32_t) (ra_dist / (int64_t) spp);
    uint32_t total_dec_steps = (uint32_t) (dec_dist / (int64_t) spp);
    uint32_t ra_steps_done = 0;
    uint32_t dec_steps_done = 0;

    /* Distances in centidegrees for ramp threshold comparisons. */
    int distance_ra_cds = (int)(motors_steps_to_deg(ra_dist) * 100.0f);
    int distance_dec_cds = (int)(motors_steps_to_deg(dec_dist) * 100.0f);

    /* Direction is constant for a slew — set DIR pins once. */
    MotorDirection ra_dir = (s_motion.ra_target > motors_state.ra_steps)
                                ? MOTOR_DIRECTION_POSITIVE
                                : MOTOR_DIRECTION_NEGATIVE;
    MotorDirection dec_dir = (s_motion.dec_target > motors_state.dec_steps)
                                ? MOTOR_DIRECTION_POSITIVE
                                : MOTOR_DIRECTION_NEGATIVE;
    motors_hw_set_direction_ra(ra_dir);
    motors_hw_set_direction_dec(dec_dir);

    int ra_sign = (ra_dir == MOTOR_DIRECTION_POSITIVE) ? 1 : -1;
    int dec_sign = (dec_dir == MOTOR_DIRECTION_POSITIVE) ? 1 : -1;

    /* Double-buffer per axis: ping = in-flight, pong = queued behind it. */
    rmt_symbol_word_t ra_buf[2][RMT_BUFFER_SYMBOLS];
    rmt_symbol_word_t dec_buf[2][RMT_BUFFER_SYMBOLS];
    uint32_t ra_num[2] = {0, 0};
    uint32_t dec_num[2] = {0, 0};
    uint32_t ra_batch[2] = {0, 0};
    uint32_t dec_batch[2] = {0, 0};
    int ra_ping = 0;
    int ra_pong = 1;
    int dec_ping = 0;
    int dec_pong = 1;

    uint32_t ra_period = MAX_STEP_PERIOD_TICKS;
    uint32_t dec_period = MAX_STEP_PERIOD_TICKS;
    int64_t last_ramp_recalc_us = 0;
    bool is_move_axis = (s_motion.active_cmd_type == MOTION_CMD_MOVE_AXIS);

    compute_slew_periods(is_move_axis, 0, 0,
                         (int64_t) total_ra_steps, (int64_t) total_dec_steps,
                         distance_ra_cds, distance_dec_cds, spp,
                         &ra_period, &dec_period);
    last_ramp_recalc_us = esp_timer_get_time();

    uint32_t ra_rem = total_ra_steps - ra_steps_done;
    uint32_t dec_rem = total_dec_steps - dec_steps_done;
    ra_batch[0] = compute_batch_size(ra_period, ra_rem);
    dec_batch[0] = compute_batch_size(dec_period, dec_rem);
    ra_batch[0] = clamp_ra_batch(motors_state.ra_steps, ra_sign, ra_batch[0], spp);
    dec_batch[0] = clamp_dec_batch(motors_state.dec_steps, dec_sign, dec_batch[0], spp);

    int64_t ra_trav_base = (int64_t)ra_sign
                           * (motors_state.ra_steps - s_motion.ra_start) / (int64_t)spp;
    int64_t dec_trav_base = (int64_t)dec_sign
                            * (motors_state.dec_steps - s_motion.dec_start) / (int64_t)spp;

    ra_num[0] = encode_axis_batch(ra_buf[0], RMT_BUFFER_SYMBOLS,
                                   ra_batch[0], ra_period, spp,
                                   motors_state.ra_speed,
                                   ra_trav_base, (int64_t) total_ra_steps, distance_ra_cds,
                                   is_move_axis);
    dec_num[0] = encode_axis_batch(dec_buf[0], RMT_BUFFER_SYMBOLS,
                                    dec_batch[0], dec_period, spp,
                                    motors_state.dec_speed,
                                    dec_trav_base, (int64_t) total_dec_steps, distance_dec_cds,
                                    is_move_axis);

    if (!s_motion.active) return;

    esp_err_t tx_err = ESP_OK;
    if (ra_batch[0] > 0) tx_err = motors_rmt_transmit_ra(ra_buf[0], ra_num[0]);
    if (dec_batch[0] > 0 && tx_err == ESP_OK)
        tx_err = motors_rmt_transmit_dec(dec_buf[0], dec_num[0]);
    if (tx_err != ESP_OK) {
        ESP_LOGE(TAG, "RMT transmit failed: %s", esp_err_to_name(tx_err));
        abort_rmt_hardware_fault();
        return;
    }

    /* Pre-encode and pre-submit the second batch while the first transmits, so
     * the next batch starts with zero CPU-dependent gap. */
    {
        int64_t ra_proj = (int64_t)ra_sign
            * (motors_state.ra_steps + (int64_t)ra_sign * (int64_t)ra_batch[0]
               - s_motion.ra_start) / (int64_t)spp;
        int64_t dec_proj = (int64_t)dec_sign
            * (motors_state.dec_steps + (int64_t)dec_sign * (int64_t)dec_batch[0]
               - s_motion.dec_start) / (int64_t)spp;

        ra_rem = total_ra_steps - ra_steps_done - ra_batch[0];
        dec_rem = total_dec_steps - dec_steps_done - dec_batch[0];
        ra_batch[1] = compute_batch_size(ra_period, ra_rem);
        dec_batch[1] = compute_batch_size(dec_period, dec_rem);
        ra_batch[1] = clamp_ra_batch(motors_state.ra_steps + (int64_t)ra_sign * (int64_t)ra_batch[0] * (int64_t)spp,
                                     ra_sign, ra_batch[1], spp);
        dec_batch[1] = clamp_dec_batch(motors_state.dec_steps + (int64_t)dec_sign * (int64_t)dec_batch[0] * (int64_t)spp,
                                       dec_sign, dec_batch[1], spp);

        ra_num[1] = encode_axis_batch(ra_buf[1], RMT_BUFFER_SYMBOLS,
                                       ra_batch[1], ra_period, spp,
                                       motors_state.ra_speed,
                                       ra_proj, (int64_t) total_ra_steps, distance_ra_cds,
                                       is_move_axis);
        dec_num[1] = encode_axis_batch(dec_buf[1], RMT_BUFFER_SYMBOLS,
                                        dec_batch[1], dec_period, spp,
                                        motors_state.dec_speed,
                                        dec_proj, (int64_t) total_dec_steps, distance_dec_cds,
                                        is_move_axis);

        if (ra_batch[1] > 0 || dec_batch[1] > 0) {
            tx_err = ESP_OK;
            if (ra_batch[1] > 0)
                tx_err = motors_rmt_transmit_no_drain_ra(ra_buf[1], ra_num[1]);
            if (dec_batch[1] > 0 && tx_err == ESP_OK)
                tx_err = motors_rmt_transmit_no_drain_dec(dec_buf[1], dec_num[1]);
            if (tx_err != ESP_OK) {
                ESP_LOGE(TAG, "RMT transmit failed: %s", esp_err_to_name(tx_err));
                abort_rmt_hardware_fault();
                return;
            }
        }
    }

    while (s_motion.active) {
        motors_test_hook_loop_top();
        /* Read the shared MoveAxis state under the admission lock so the checks
         * below are coherent, not racy volatile reads. */
        uint32_t setpoint_version = 0;
        bool manual = false;
        uint32_t session = 0;
        if (is_move_axis) {
            portENTER_CRITICAL(&motors_admission_lock);
            setpoint_version = motors_move_axis_version;
            manual = motors_manual_move;
            session = motors_manual_session;
            portEXIT_CRITICAL(&motors_admission_lock);
        }

        /* The manual session was finalised or replaced: drain gracefully (not
         * abort) so the committed batches are accounted and the session's own
         * completion can be confirmed.  Checked before the version preempt below:
         * a replacement session bumps the version too. */
        if (is_move_axis && (!manual || session != s_motion.move_axis_session)) {
            s_motion.active = false;
            s_motion.graceful = true;
            break;
        }

        /* A newer setpoint within the same session preempts this one: drain the
         * committed batches and let the main loop re-read the setpoint. */
        if (is_move_axis && setpoint_version != s_motion.move_axis_version) {
            s_motion.active = false;
            s_motion.preempted = true;
            break;
        }

        /* Recompute ramp periods (throttled to ~5 ms). */
        int64_t now = esp_timer_get_time();
        if (now - last_ramp_recalc_us > 5000) {
            int64_t ra_proj_trav = (int64_t)ra_sign
                * (motors_state.ra_steps + (int64_t)ra_sign * (int64_t)ra_batch[ra_pong]
                   - s_motion.ra_start) / (int64_t)spp;
            int64_t dec_proj_trav = (int64_t)dec_sign
                * (motors_state.dec_steps + (int64_t)dec_sign * (int64_t)dec_batch[dec_pong]
                   - s_motion.dec_start) / (int64_t)spp;
            compute_slew_periods(is_move_axis,
                                 ra_proj_trav, dec_proj_trav,
                                 (int64_t) total_ra_steps, (int64_t) total_dec_steps,
                                 distance_ra_cds, distance_dec_cds, spp,
                                 &ra_period, &dec_period);
            last_ramp_recalc_us = now;
        }

        /* Advance RA when its in-flight batch completes. */
        if (ra_steps_done < total_ra_steps
            && ra_batch[ra_ping] > 0
            && motors_rmt_try_wait_ra()) {
            portENTER_CRITICAL(&motors_state_lock);
            motors_state.ra_steps += (int64_t)ra_sign * (int64_t)ra_batch[ra_ping] * (int64_t)spp;
            portEXIT_CRITICAL(&motors_state_lock);
            ra_steps_done += ra_batch[ra_ping];
            ra_batch[ra_ping] = 0;
            if (!s_motion.active) break;

            int64_t ra_proj_trav = (int64_t)ra_sign
                * (motors_state.ra_steps + (int64_t)ra_sign * (int64_t)ra_batch[ra_pong]
                   - s_motion.ra_start) / (int64_t)spp;
            ra_rem = total_ra_steps - ra_steps_done - ra_batch[ra_pong];
            ra_batch[ra_ping] = compute_batch_size(ra_period, ra_rem);
            ra_batch[ra_ping] = clamp_ra_batch(motors_state.ra_steps + (int64_t)ra_sign * (int64_t)ra_batch[ra_pong] * (int64_t)spp,
                                               ra_sign, ra_batch[ra_ping], spp);
            ra_num[ra_ping] = encode_axis_batch(ra_buf[ra_ping], RMT_BUFFER_SYMBOLS,
                                                 ra_batch[ra_ping], ra_period, spp,
                                                 motors_state.ra_speed,
                                                 ra_proj_trav, (int64_t) total_ra_steps, distance_ra_cds,
                                                 is_move_axis);
            if (ra_batch[ra_ping] > 0) {
                tx_err = motors_rmt_transmit_no_drain_ra(ra_buf[ra_ping], ra_num[ra_ping]);
                if (tx_err != ESP_OK) {
                    ESP_LOGE(TAG, "RMT transmit failed: %s", esp_err_to_name(tx_err));
                    abort_rmt_hardware_fault();
                    break;
                }
            }
            { int t = ra_ping; ra_ping = ra_pong; ra_pong = t; }
        }

        /* Advance DEC when its in-flight batch completes. */
        if (dec_steps_done < total_dec_steps
            && dec_batch[dec_ping] > 0
            && motors_rmt_try_wait_dec()) {
            portENTER_CRITICAL(&motors_state_lock);
            motors_state.dec_steps += (int64_t)dec_sign * (int64_t)dec_batch[dec_ping] * (int64_t)spp;
            portEXIT_CRITICAL(&motors_state_lock);
            dec_steps_done += dec_batch[dec_ping];
            dec_batch[dec_ping] = 0;
            if (!s_motion.active) break;

            int64_t dec_proj_trav = (int64_t)dec_sign
                * (motors_state.dec_steps + (int64_t)dec_sign * (int64_t)dec_batch[dec_pong]
                   - s_motion.dec_start) / (int64_t)spp;
            dec_rem = total_dec_steps - dec_steps_done - dec_batch[dec_pong];
            dec_batch[dec_ping] = compute_batch_size(dec_period, dec_rem);
            dec_batch[dec_ping] = clamp_dec_batch(motors_state.dec_steps + (int64_t)dec_sign * (int64_t)dec_batch[dec_pong] * (int64_t)spp,
                                                  dec_sign, dec_batch[dec_ping], spp);
            dec_num[dec_ping] = encode_axis_batch(dec_buf[dec_ping], RMT_BUFFER_SYMBOLS,
                                                   dec_batch[dec_ping], dec_period, spp,
                                                   motors_state.dec_speed,
                                                   dec_proj_trav, (int64_t) total_dec_steps, distance_dec_cds,
                                                   is_move_axis);
            if (dec_batch[dec_ping] > 0) {
                tx_err = motors_rmt_transmit_no_drain_dec(dec_buf[dec_ping], dec_num[dec_ping]);
                if (tx_err != ESP_OK) {
                    ESP_LOGE(TAG, "RMT transmit failed: %s", esp_err_to_name(tx_err));
                    abort_rmt_hardware_fault();
                    break;
                }
            }
            { int t = dec_ping; dec_ping = dec_pong; dec_pong = t; }
        }

        if (!check_motion_conditions()) break;
        if (!s_motion.active) break;

        if (ra_steps_done >= total_ra_steps && dec_steps_done >= total_dec_steps) {
            if (slew_residual_correction(ra_dist, dec_dist, total_ra_steps, total_dec_steps,
                                         ra_sign, dec_sign, spp)) {
                finish_motion();
            }
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }

    /* Graceful STOP or a setpoint preemption: account the in-flight + queued
     * batches already submitted to RMT, so the position stays exact.  A drain
     * that cannot complete degrades to an abort. */
    if ((s_motion.graceful || s_motion.preempted) && !s_motion.aborted) {
        int64_t deadline_us = esp_timer_get_time() + MOTION_DRAIN_TIMEOUT_US;
        bool ra_ok = drain_axis_batches(ra_batch, ra_ping, ra_pong, ra_sign, spp,
                                        &motors_state.ra_steps, motors_rmt_try_wait_ra,
                                        deadline_us);
        bool dec_ok = drain_axis_batches(dec_batch, dec_ping, dec_pong, dec_sign, spp,
                                         &motors_state.dec_steps, motors_rmt_try_wait_dec,
                                         deadline_us);
        if (!ra_ok || !dec_ok) {
            s_motion.aborted = true;
        }
    } else if (s_motion.aborted) {
        /* Urgent abort: consume only the completions already available (deadline
         * 0 makes the drain non-blocking); unknown partially-emitted batches
         * surface as position_uncertain. */
        (void) drain_axis_batches(ra_batch, ra_ping, ra_pong, ra_sign, spp,
                                  &motors_state.ra_steps, motors_rmt_try_wait_ra,
                                  0);
        (void) drain_axis_batches(dec_batch, dec_ping, dec_pong, dec_sign, spp,
                                  &motors_state.dec_steps, motors_rmt_try_wait_dec,
                                  0);
    }
}

/* =========================================================================
 * Tracking and PulseGuide — fractional-accumulator RMT pulse
 * ========================================================================= */

/* Wait for one tracking step's completion, interrupting early on an urgent fault
 * (same predicate as check_motion_conditions).  A normal STOP is not an
 * interrupt: the committed pulse's completion is allowed to arrive within the
 * deadline. */
static esp_err_t wait_tracking_step_done(esp_err_t (*wait)(TickType_t)) {
    const int64_t deadline_us = esp_timer_get_time() + TRACKING_STEP_TIMEOUT_US;
    while (true) {
        esp_err_t e = wait(pdMS_TO_TICKS(TRACKING_STEP_SLICE_MS));
        if (e == ESP_OK) return ESP_OK;
        if (motors_status_is_error(motors_state.status)) return ESP_ERR_TIMEOUT;
        if (esp_timer_get_time() > deadline_us) return ESP_ERR_TIMEOUT;
    }
}

/* A tracking step was submitted but its completion never arrived: the pulse may
 * or may not have been emitted, so the position is untrustworthy until a
 * re-reference. */
static void mark_tracking_position_uncertain(void) {
    portENTER_CRITICAL(&motors_state_lock);
    motors_state.position_uncertain = true;
    portEXIT_CRITICAL(&motors_state_lock);
}

/* Emit a single RA tracking step in the given direction.  Returns false when the
 * motion must abort (limit hit, RMT error). */
static bool emit_ra_tracking_step(int direction, rmt_symbol_word_t *buffer) {
    int64_t next = motors_state.ra_steps + (int64_t)direction;

    if (!motors_is_valid_ra_steps(next)) {
        ESP_LOGW(TAG, "RA limit at %.3f deg",
                 (double)motors_steps_to_deg(motors_state.ra_steps));
        s_motion.active = false;
        /* A limit hit ends the tracking base and any in-flight guide.  Clear the
         * deadlines so the post-loop guiding recompute sees them expired, and
         * flag truncation if a guide was actually cut short. */
        bool any_guide = (s_motion.ra_guide_deadline_us != 0
                          || s_motion.dec_guide_deadline_us != 0);
        clear_guide_state();
        portENTER_CRITICAL(&motors_state_lock);
        if (motors_state.status != MOTORS_STATUS_ERROR) {
            motors_state.status = MOTORS_STATUS_READY;
        }
        motors_state.tracking = TRACKING_NONE;
        motors_state.guiding = false;
        if (any_guide) {
            motors_state.guide_truncated = true;
        }
        portEXIT_CRITICAL(&motors_state_lock);
        return false;
    }

    if (!s_motion.active)
        return false;

    motors_rmt_encode_pulse(buffer);
    esp_err_t tx_err = motors_rmt_transmit_ra(buffer, 1);
    if (tx_err != ESP_OK) {
        ESP_LOGE(TAG, "RMT RA tx fail: %s", esp_err_to_name(tx_err));
        abort_rmt_hardware_fault();
        return false;
    }

    esp_err_t wait_err = wait_tracking_step_done(motors_rmt_wait_ra);
    if (wait_err != ESP_OK) {
        mark_tracking_position_uncertain();
        abort_rmt_hardware_fault();
        return false;
    }

    /* Account the confirmed step even if STOP arrived during the wait: a pulse
     * whose completion was consumed must advance the counter exactly once. */
    portENTER_CRITICAL(&motors_state_lock);
    motors_state.ra_steps = next;
    portEXIT_CRITICAL(&motors_state_lock);

    if (!s_motion.active)
        return false;

    return true;
}

/* Open-ended tracking: integrate elapsed time into a phase accumulator and emit
 * one STEP per whole phase unit.  Only RA is stepped during tracking; DEC
 * velocity is zero.  A guide offset folds into the per-interval velocity. */
static void tracking_loop_rmt(void) {
    const int64_t FINE_MARGIN_US = 2000;
    const float deg_per_step = motors_get_deg_per_microstep();

    double ra_phase = 0.0;
    double dec_phase = 0.0;
    int64_t last_time_us = esp_timer_get_time();
    int64_t last_check_us = last_time_us;

    int ra_dir_set = 0;   /* 0 = unknown, ±1 = last set direction */
    int dec_dir_set = 0;
    rmt_symbol_word_t ra_sym[RMT_BUFFER_SYMBOLS];
    rmt_symbol_word_t dec_sym[RMT_BUFFER_SYMBOLS];

    /* Effective velocities, persisted so the phase integral at the top of each
     * iteration uses the velocity actually in effect during the elapsed
     * interval, before any guide start/end at `now`. */
    float ra_vel = (motors_state.status == MOTORS_STATUS_TRACKING)
                   ? motors_state.ra_speed : 0.0f;
    float dec_vel = 0.0f;

    while (s_motion.active) {
        int64_t now = esp_timer_get_time();
        double dt_s = (double)(now - last_time_us) / 1000000.0;
        last_time_us = now;

        ra_phase += (double)ra_vel * dt_s / (double)deg_per_step;
        dec_phase += (double)dec_vel * dt_s / (double)deg_per_step;

        /* Drain queued guiding and tracking-mode updates inline (the task never
         * returns to xQueueReceive during tracking).  SLEW and MOVE_AXIS must not
         * run inline. */
        {
            MotionCommand queued;
            while (xQueuePeek(motion_cmd_queue, &queued, 0) == pdTRUE
                   && (queued.type == MOTION_CMD_PULSE_GUIDE
                       || queued.type == MOTION_CMD_TRACK)) {
                xQueueReceive(motion_cmd_queue, &queued, 0);
                process_command(queued);
            }
        }

        /* A MOVE_AXIS or SLEW accepted while tracking ends this loop so the main
         * loop dequeues and dispatches it. */
        {
            MotionCommand queued;
            if (xQueuePeek(motion_cmd_queue, &queued, 0) == pdTRUE
                && (queued.type == MOTION_CMD_MOVE_AXIS
                    || queued.type == MOTION_CMD_SLEW)) {
                s_motion.active = false;
                break;
            }
        }

        /* Guide expiry — checked every iteration for µs-precise termination. */
        if (s_motion.ra_guide_deadline_us
            && now >= s_motion.ra_guide_deadline_us) {
            s_motion.ra_guide_deadline_us = 0;
            s_motion.ra_guide_offset_dps = 0.0f;
        }
        if (s_motion.dec_guide_deadline_us
            && now >= s_motion.dec_guide_deadline_us) {
            s_motion.dec_guide_deadline_us = 0;
            s_motion.dec_guide_offset_dps = 0.0f;
            dec_dir_set = 0;
        }
        motors_state.guiding = (s_motion.ra_guide_deadline_us != 0
                                || s_motion.dec_guide_deadline_us != 0);

        /* Velocities for the interval starting at `now`. */
        ra_vel = (motors_state.status == MOTORS_STATUS_TRACKING)
                 ? motors_state.ra_speed : 0.0f;
        if (s_motion.ra_guide_deadline_us)
            ra_vel += s_motion.ra_guide_offset_dps;
        dec_vel = 0.0f;
        if (s_motion.dec_guide_deadline_us)
            dec_vel = s_motion.dec_guide_offset_dps;

        /* Conditions check (throttled to ~500 µs). */
        if (now - last_check_us >= 500) {
            last_check_us = now;
            if (!check_motion_conditions()) break;
            if (motors_state.status != MOTORS_STATUS_TRACKING
                && !motors_state.guiding) {
                s_motion.active = false;
                break;
            }
        }

        /* Emit RA steps. */
        while (ra_phase >= 1.0) {
            if (ra_dir_set != 1) {
                motors_hw_set_direction_ra(MOTOR_DIRECTION_POSITIVE);
                ra_dir_set = 1;
            }
            if (!emit_ra_tracking_step(1, ra_sym)) {
                ra_phase = 0.0;
                return;
            }
            ra_phase -= 1.0;
        }
        while (ra_phase <= -1.0) {
            if (ra_dir_set != -1) {
                motors_hw_set_direction_ra(MOTOR_DIRECTION_NEGATIVE);
                ra_dir_set = -1;
            }
            if (!emit_ra_tracking_step(-1, ra_sym)) {
                ra_phase = 0.0;
                return;
            }
            ra_phase += 1.0;
        }

        /* Emit DEC steps. */
        while (dec_phase >= 1.0) {
            if (dec_dir_set != 1) {
                motors_hw_set_direction_dec(MOTOR_DIRECTION_POSITIVE);
                dec_dir_set = 1;
            }
            int64_t next = motors_state.dec_steps + 1;
            if (!motors_is_valid_dec_steps(next)) {
                ESP_LOGW(TAG, "DEC limit at %.3f deg",
                         (double)motors_steps_to_deg(motors_state.dec_steps));
                motors_state.guide_truncated = true;
                s_motion.dec_guide_deadline_us = 0;
                s_motion.dec_guide_offset_dps = 0.0f;
                dec_dir_set = 0;
                dec_phase = 0.0;
                break;
            }
            if (!s_motion.active) { dec_phase = 0.0; return; }

            motors_rmt_encode_pulse(dec_sym);
            esp_err_t tx_err = motors_rmt_transmit_dec(dec_sym, 1);
            if (tx_err != ESP_OK) {
                ESP_LOGE(TAG, "RMT DEC tx fail: %s", esp_err_to_name(tx_err));
                abort_rmt_hardware_fault();
                dec_phase = 0.0;
                return;
            }
            esp_err_t wait_err = wait_tracking_step_done(motors_rmt_wait_dec);
            if (wait_err != ESP_OK) {
                mark_tracking_position_uncertain();
                abort_rmt_hardware_fault();
                dec_phase = 0.0;
                return;
            }
            portENTER_CRITICAL(&motors_state_lock);
            motors_state.dec_steps = next;
            portEXIT_CRITICAL(&motors_state_lock);
            dec_phase -= 1.0;
            if (!s_motion.active) { dec_phase = 0.0; return; }
        }
        while (dec_phase <= -1.0) {
            if (dec_dir_set != -1) {
                motors_hw_set_direction_dec(MOTOR_DIRECTION_NEGATIVE);
                dec_dir_set = -1;
            }
            int64_t next = motors_state.dec_steps - 1;
            if (!motors_is_valid_dec_steps(next)) {
                ESP_LOGW(TAG, "DEC limit at %.3f deg",
                         (double)motors_steps_to_deg(motors_state.dec_steps));
                motors_state.guide_truncated = true;
                s_motion.dec_guide_deadline_us = 0;
                s_motion.dec_guide_offset_dps = 0.0f;
                dec_dir_set = 0;
                dec_phase = 0.0;
                break;
            }
            if (!s_motion.active) { dec_phase = 0.0; return; }

            motors_rmt_encode_pulse(dec_sym);
            esp_err_t tx_err = motors_rmt_transmit_dec(dec_sym, 1);
            if (tx_err != ESP_OK) {
                ESP_LOGE(TAG, "RMT DEC tx fail: %s", esp_err_to_name(tx_err));
                abort_rmt_hardware_fault();
                dec_phase = 0.0;
                return;
            }
            esp_err_t wait_err = wait_tracking_step_done(motors_rmt_wait_dec);
            if (wait_err != ESP_OK) {
                mark_tracking_position_uncertain();
                abort_rmt_hardware_fault();
                dec_phase = 0.0;
                return;
            }
            portENTER_CRITICAL(&motors_state_lock);
            motors_state.dec_steps = next;
            portEXIT_CRITICAL(&motors_state_lock);
            dec_phase += 1.0;
            if (!s_motion.active) { dec_phase = 0.0; return; }
        }

        /* Time to the next step boundary, then sleep (blocking, not busy-wait,
         * so high-rate guiding never starves lower-priority tasks). */
        double ra_remaining = (ra_vel > 0.0) ? (1.0 - ra_phase)
                                             : (1.0 + ra_phase);
        double dec_remaining = (dec_vel > 0.0) ? (1.0 - dec_phase)
                                               : (1.0 + dec_phase);
        double ra_s = (ra_vel != 0.0) ? ra_remaining * deg_per_step / fabs((double)ra_vel) : 1e9;
        double dec_s = (dec_vel != 0.0) ? dec_remaining * deg_per_step / fabs((double)dec_vel) : 1e9;
        int64_t w_us = (int64_t)((ra_s < dec_s ? ra_s : dec_s) * 1000000.0);
        if (w_us < 0) w_us = 0;

        /* Wake at the earliest guide deadline so a pulse ends exactly on time. */
        {
            int64_t now_us = esp_timer_get_time();
            if (s_motion.ra_guide_deadline_us) {
                int64_t until = s_motion.ra_guide_deadline_us - now_us;
                if (until < w_us) w_us = until;
            }
            if (s_motion.dec_guide_deadline_us) {
                int64_t until = s_motion.dec_guide_deadline_us - now_us;
                if (until < w_us) w_us = until;
            }
            if (w_us < 0) w_us = 0;
        }

        if (w_us > FINE_MARGIN_US) {
            uint32_t sm = (uint32_t)((w_us - FINE_MARGIN_US) / 1000);
            if (sm > 50) sm = 50;
            if (sm < 1) sm = 1;
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(sm));
            continue;
        }
        if (w_us > 0) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1));
        }
    }
}

/* =========================================================================
 * Motion lifecycle and finalization
 * ========================================================================= */

/* Dispatch the active command to the right loop, then finalise. */
static void motion_loop(void) {
    if (motors_status_is_error(motors_state.status)) {
        return;
    }

    /* Synchronize the start with STOP: read `active` and reset
     * aborted/graceful/preempted together under the admission lock, so a STOP
     * that landed after process_command() set active=true is seen coherently. */
    portENTER_CRITICAL(&motors_admission_lock);
    bool stopped = !s_motion.active;
    if (!stopped) {
        s_motion.aborted = false;
        s_motion.graceful = false;
        s_motion.preempted = false;
    }
    portEXIT_CRITICAL(&motors_admission_lock);
    if (stopped) {
        return;
    }

    /* Select the microstep resolution BEFORE any STEP pulse is emitted.  A
     * failed switch stops the mount and puts it into ERROR. */
    int64_t ra_dist = 0;
    int64_t dec_dist = 0;
    if (s_motion.active_cmd_type == MOTION_CMD_SLEW) {
        ra_dist = s_motion.ra_target - s_motion.ra_start;
        dec_dist = s_motion.dec_target - s_motion.dec_start;
    }
    if (!motors_microsteps_prepare(s_motion.active_cmd_type, ra_dist, dec_dist)) {
        return;
    }

    s_motion.ran = true;

    if ((motors_state.status == MOTORS_STATUS_TRACKING
         && motors_state.tracking != TRACKING_NONE)
        || motors_state.guiding) {
        tracking_loop_rmt();
    } else {
        slewing_loop_rmt();

        /* A slew enqueued while tracking carries the mode to restore.  Resume
         * inline so the transition is atomic; an aborted slew cancels it. */
        if (!s_motion.aborted && !s_motion.graceful
            && s_motion.resume_tracking != TRACKING_NONE) {
            TrackingMode resume = s_motion.resume_tracking;
            s_motion.resume_tracking = TRACKING_NONE;

            /* A coarse slew left the drivers coarse; tracking runs fine. */
            if (!motors_microsteps_prepare(MOTION_CMD_TRACK, 0, 0)) {
                return;
            }

            begin_tracking(resume);
            tracking_loop_rmt();
        }
    }

    /* Recreate the RMT channels only after an external abort (STOP/PARK/preempt):
     * the one case where a queued double-buffered batch must be dropped. */
    if (s_motion.aborted) {
        motors_rmt_reset_both();

        /* An aborted batched move drops a fraction of the in-flight batch, so the
         * step counters may be off by up to one batch.  Surface that uncertainty;
         * the tracking loop is excluded (it accounts one step per transaction). */
        if (s_motion.active_cmd_type == MOTION_CMD_SLEW
            || s_motion.active_cmd_type == MOTION_CMD_MOVE_AXIS) {
            portENTER_CRITICAL(&motors_state_lock);
            motors_state.position_uncertain = true;
            portEXIT_CRITICAL(&motors_state_lock);
        }
    }

    /* The drain result is final here.  Capture its identity and motive (graceful/
     * aborted) at the true finalisation point, under the admission lock, so a STOP
     * landing before the barrier cannot re-tag it with a foreign generation. */
    portENTER_CRITICAL(&motors_admission_lock);
    s_motion.drain_generation = motors_stop_generation;
    s_motion.drain_graceful = s_motion.graceful;
    portEXIT_CRITICAL(&motors_admission_lock);
    s_motion.drain_aborted = s_motion.aborted;
    motors_test_hook_after_motion_loop();
}

/* =========================================================================
 * Task entry
 * ========================================================================= */

static void motors_motion_task_run(void *arg) {
    (void) arg;

    /* Initialize RMT from this task — pins the RMT ISR to core 1. */
    esp_err_t rmt_err = motors_rmt_init();
    if (rmt_err != ESP_OK) {
        ESP_LOGE(TAG, "motors_rmt_init failed: %s — entering ERROR state",
                 esp_err_to_name(rmt_err));
        motors_enter_hardware_fault();
        motors_motion_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }
    while (true) {
        /* Publish the stop outcome BEFORE signalling idle.  The rest generation
         * is read here so a STOP landing while the task is held is still
         * confirmed at the current generation. */
        motors_test_hook_before_idle_publish();
        uint32_t rest_gen = motors_stop_generation_get();
        portENTER_CRITICAL(&motors_state_lock);
        s_idle_generation = rest_gen;
        if (s_motion.ran) {
            /* Use the snapshot captured at the end of motion_loop, not the live
             * flags, so a STOP after the motion ended cannot rewrite the reason. */
            bool graceful = s_motion.drain_graceful;
            bool motion_aborted = s_motion.drain_aborted;
            s_idle_aborted_generation = s_motion.drain_generation;
            s_idle_aborted = graceful && motion_aborted;
            s_motion.aborted = false;
            s_motion.ran = false;
            /* A MoveAxis whose session was finalised (graceful) publishes READY
             * and the explicit session completion here.  A setpoint preemption
             * (not graceful) drains without finalising. */
            if (s_motion.active_cmd_type == MOTION_CMD_MOVE_AXIS) {
                if (graceful) {
                    if (!motion_aborted
                        && motors_state.status == MOTORS_STATUS_SLEWING) {
                        motors_state.status = MOTORS_STATUS_READY;
                    }
                    if (s_motion.move_axis_session > s_finished_manual_session) {
                        s_finished_manual_session = s_motion.move_axis_session;
                        s_finished_manual_aborted = motion_aborted;
                    }
                }
                if (motion_aborted) {
                    /* The drain failed (position uncertain): close admission at
                     * the point of detection.  Idempotent. */
                    motors_admission_close();
                }
            }
        }
        /* Publish READY for a motion that just completed. */
        if (s_motion.completed) {
            if (motors_state.status == MOTORS_STATUS_SLEWING
                || (motors_state.status == MOTORS_STATUS_TRACKING
                    && motors_state.tracking == TRACKING_NONE)) {
                motors_state.status = MOTORS_STATUS_READY;
                motors_state.tracking = TRACKING_NONE;
            }
            s_motion.completed = false;
        }
        s_motion_idle = true;
        portEXIT_CRITICAL(&motors_state_lock);

        MotionCommand cmd;
        /* Block for at most MOTION_IDLE_QUEUE_POLL_MS so a STOP landing while
         * idle re-publishes the idle generation instead of sleeping past it. */
        if (xQueueReceive(motion_cmd_queue, &cmd,
                          pdMS_TO_TICKS(MOTION_IDLE_QUEUE_POLL_MS)) != pdTRUE) {
            continue;
        }

        portENTER_CRITICAL(&motors_state_lock);
        s_motion_idle = false;
        portEXIT_CRITICAL(&motors_state_lock);

        process_command(cmd);

        motors_test_hook_after_process_command();

        if (cmd.type == MOTION_CMD_SLEW || cmd.type == MOTION_CMD_TRACK ||
            cmd.type == MOTION_CMD_MOVE_AXIS) {
            motion_loop();
        }
        /* Standalone guide (not tracking): run a short motion loop for the pulse. */
        if (cmd.type == MOTION_CMD_PULSE_GUIDE
            && motors_state.status != MOTORS_STATUS_TRACKING) {
            motion_loop();
        }

        /* Clear the guiding flag when both axes are idle. */
        bool ra_active = (s_motion.ra_guide_deadline_us != 0)
                      && (esp_timer_get_time() < s_motion.ra_guide_deadline_us);
        bool dec_active = (s_motion.dec_guide_deadline_us != 0)
                       && (esp_timer_get_time() < s_motion.dec_guide_deadline_us);
        motors_state.guiding = (ra_active || dec_active);
    }
}

/* =========================================================================
 * Public API
 * ========================================================================= */

esp_err_t motors_motion_task_init(void) {
    if (xTaskCreatePinnedToCore(
        motors_motion_task_run,
        "motors_motion",
        MOTION_TASK_STACK_WORDS,
        NULL,
        MOTION_TASK_PRIORITY,
        &motors_motion_task_handle,
        1) != pdPASS) {  /* dedicated core — CPU 1 is reserved for motion */
        ESP_LOGE(TAG, "Failed to create motion task");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

/* Wait (bounded) until the motion task confirms rest at `generation` or later,
 * returning the confirmation and the abort result in one coherent read. */
bool motors_motion_wait_idle(uint32_t generation, bool *aborted) {
    const int64_t deadline_us = esp_timer_get_time() + MOTION_IDLE_TIMEOUT_US;
    while (true) {
        bool idle;
        uint32_t idle_gen;
        bool idle_aborted;
        uint32_t idle_aborted_gen;
        portENTER_CRITICAL(&motors_state_lock);
        idle = s_motion_idle;
        idle_gen = s_idle_generation;
        idle_aborted = s_idle_aborted;
        idle_aborted_gen = s_idle_aborted_generation;
        if (idle && idle_gen >= generation) {
            /* The drain result is attributed only to the generation it belongs to:
             * a later generation's confirmation never hands a stale result. */
            portEXIT_CRITICAL(&motors_state_lock);
            if (aborted != NULL) {
                *aborted = (idle_aborted_gen == generation) ? idle_aborted : false;
            }
            return true;
        }
        portEXIT_CRITICAL(&motors_state_lock);

        if (esp_timer_get_time() >= deadline_us) {
            ESP_LOGW(TAG, "motion task did not confirm rest at generation %lu",
                     (unsigned long) generation);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

/* Wait (bounded) until the executor publishes the explicit completion of the
 * MoveAxis session `session` (or a later one). */
bool motors_motion_wait_drained(uint32_t session, bool *aborted) {
    const int64_t deadline_us = esp_timer_get_time() + MOTION_IDLE_TIMEOUT_US;
    while (true) {
        uint32_t finished_session;
        bool finished_aborted;
        portENTER_CRITICAL(&motors_state_lock);
        finished_session = s_finished_manual_session;
        finished_aborted = s_finished_manual_aborted;
        if (finished_session >= session) {
            portEXIT_CRITICAL(&motors_state_lock);
            if (aborted != NULL) {
                *aborted = finished_aborted;
            }
            return true;
        }
        portEXIT_CRITICAL(&motors_state_lock);

        if (esp_timer_get_time() >= deadline_us) {
            ESP_LOGW(TAG, "motion task did not confirm manual session completion");
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

/* Observability: the last published manual-session completion (monotonic). */
uint32_t motors_finished_manual_session(void) {
    uint32_t session;
    portENTER_CRITICAL(&motors_state_lock);
    session = s_finished_manual_session;
    portEXIT_CRITICAL(&motors_state_lock);
    return session;
}

bool motors_finished_manual_aborted(void) {
    bool aborted;
    portENTER_CRITICAL(&motors_state_lock);
    aborted = s_finished_manual_aborted;
    portEXIT_CRITICAL(&motors_state_lock);
    return aborted;
}

/* Observability: the last published global stop confirmation. */
uint32_t motors_idle_generation(void) {
    uint32_t gen;
    portENTER_CRITICAL(&motors_state_lock);
    gen = s_idle_generation;
    portEXIT_CRITICAL(&motors_state_lock);
    return gen;
}

bool motors_idle_aborted(void) {
    bool aborted;
    portENTER_CRITICAL(&motors_state_lock);
    aborted = s_idle_aborted;
    portEXIT_CRITICAL(&motors_state_lock);
    return aborted;
}

/* The generation the last published drain result belongs to. */
uint32_t motors_idle_aborted_generation(void) {
    uint32_t gen;
    portENTER_CRITICAL(&motors_state_lock);
    gen = s_idle_aborted_generation;
    portEXIT_CRITICAL(&motors_state_lock);
    return gen;
}

/* Test-only: clear the stable drain-result record.  Production never clears it
 * mid-session. */
void motors_reset_idle_completion(void) {
    portENTER_CRITICAL(&motors_state_lock);
    s_idle_aborted = false;
    s_idle_aborted_generation = 0;
    portEXIT_CRITICAL(&motors_state_lock);
}
