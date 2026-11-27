/* Motors - motors_motion_task.c
 *
 * Purpose: FreeRTOS motion task — consumes MotionCommands from the queue
 * and drives axis positions via RMT+DMA step pulse generation.
 *
 * The motion task is the sole writer of motors_state position fields
 * (ra_steps, dec_steps) during slews and tracking.
 * Status and tracking fields are updated cooperatively by the motion
 * task, motors_stop(), and motors_park().
 *
 * Two execution paths, dispatched by command type:
 *   slewing_loop_rmt  — distance-bounded, ramped accel/decel, batched RMT
 *   tracking_loop_rmt — open-ended, constant velocity, fractional accumulator
 *
 * RMT replaces the previous software GPIO bit-banging. Step pulse shapes
 * are hardware-timed; the pulse start still depends on the scheduler.
 * The CPU sleeps on a semaphore while the RMT peripheral streams symbols.
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

/* Test hook — a weak no-op overridden by the host harness to pause the motion
 * task after a command is dequeued and validated but before its motion loop
 * activates, so a STOP can land deterministically in that window. */
void __attribute__((weak)) motors_test_hook_after_process_command(void) {}
void __attribute__((weak)) motors_test_hook_before_move_axis_saved_tracking_publish(void) {}
/* Test hook — pause the executor at the idle barrier, before it publishes the
 * stop confirmation.  Lets a test hold the confirmation so a STOP's bounded wait
 * times out, then release it to publish the (late) result deterministically. */
void __attribute__((weak)) motors_test_hook_before_idle_publish(void) {}
/* Test hook — pause the executor inside drain_axis_batches()'s wait branch (no
 * completion available), so a test can deterministically land a hardware fault
 * while the executor is draining and verify the drain is interrupted by the
 * fault rather than by the deadline. */
void __attribute__((weak)) motors_test_hook_drain_wait(void) {}
/* Test hook — pause the executor at the top of the slewing main loop, before it
 * reads the session or consumes any completion.  Lets a test park the executor,
 * then publish a completion + partial emission + fault so the next loop iteration
 * handles them deterministically. */
void __attribute__((weak)) motors_test_hook_loop_top(void) {}
/* Test hook — pause the executor at the end of motion_loop(), right after the
 * drain result is final and its identity (drain_generation) has been captured,
 * but before the idle barrier publishes it.  Lets a test exercise a preemption
 * window between drain finalisation and barrier publication. */
void __attribute__((weak)) motors_test_hook_after_motion_loop(void) {}

/*
 * Task stack size — sized to accommodate batch encoding loops,
 * condition checks, and FreeRTOS queue operations.
 */
#define MOTION_TASK_STACK_WORDS 6144  /* 24 KB — room for 2×256-symbol ping-pong buffers */
#define MOTION_TASK_PRIORITY    23  /* near real-time */

/* Step period ceiling in RMT ticks — used when velocity is zero or unknown. */
#define MAX_STEP_PERIOD_TICKS (20U * 1000U * 1000U) /* 10 s at 2 MHz */

/* --------------------------------------------------------------------------
 * RMT batch control
 * -------------------------------------------------------------------------- */

/*
 * Target batch duration in RMT ticks.  Shorter batches give finer ramp
 * granularity but increase CPU overhead.  40k ticks = 20 ms at 2 MHz.
 */
#define RMT_BATCH_TARGET_TICKS  320000U

/*
 * Buffer and batch limits — unified for both axes.
 * 256 symbols (1 KB) fits one GDMA descriptor, batch ≤ 100 steps.
 *
 * RMT_BATCH_MAX_STEPS also bounds the position error from an aborted
 * motion: a STOP that interrupts a batch mid-emission leaves at most this
 * many microsteps (≈ 33.75″) uncounted.  Accepted — STOP during a slew is
 * rare and the error stays bounded, so no position-invalid flag is kept.
 */
#define RMT_BUFFER_SYMBOLS  256U
#define RMT_BATCH_MAX_STEPS  100U

/*
 * Stop/drain timeouts — derived from the maximum committed RMT transmission.
 *
 * The worst in-flight work is two batches (ping + pong) per axis, each at most
 * RMT_BATCH_MAX_STEPS pulses.  At the slowest coarse slew (min speed 0.8 °/s,
 * coarse 8 µsteps) one pulse is ~0.94 ms on the NRM 300:1 scale, so 2 × 100
 * pulses ≈ 190 ms.  500 ms is a ~2.6× margin over that bound.
 */
#define MOTION_DRAIN_TIMEOUT_US  500000

/*
 * The idle confirmation must outlast the drain deadline plus the loop-exit
 * teardown (RMT reset on abort) and the ≤ MOTION_IDLE_QUEUE_POLL_MS idle
 * re-publish latency.  1 s covers the 500 ms drain with the same margin again.
 */
#define MOTION_IDLE_TIMEOUT_US  1000000

/*
 * Idle queue poll period: the task blocks on the command queue for at most this
 * long, so a STOP that lands while the task is already idle re-publishes the
 * idle generation (confirming rest at the new generation) on the next wake
 * instead of sleeping past it.
 */
#define MOTION_IDLE_QUEUE_POLL_MS 100

TaskHandle_t motors_motion_task_handle = NULL;

/* Stop epoch — bumped on every stop (atomically with admission, see
 * motors_stop_begin) so commands enqueued before the stop are discarded by the
 * motion task. */
volatile uint32_t motors_stop_generation = 0;

/* Idle flag: true while the motion task is blocked on the command queue (not
 * processing a command), false while it runs a motion loop. */
static volatile bool s_motion_idle = true;

/* Completion record published by the motion task (under motors_state_lock) as
 * it goes idle: the stop generation at which it is now at rest, and whether the
 * motion it just left ended in an abort.  motors_motion_wait_idle() reads this
 * coherently, so a stop can confirm rest at its own generation and read the
 * abort outcome without relying on a per-motion flag left over from an earlier
 * move.
 *
 * Two distinct identities live here:
 *   s_idle_generation          — the generation at which the executor is now at
 *                                rest (re-published every barrier so a STOP that
 *                                lands while already idle is confirmed on the
 *                                next poll).  Monotonic.
 *   s_idle_aborted_generation  — the generation the drain result below belongs
 *                                to, published only when a motion actually ends
 *                                (s_motion.ran).  A confirmation once published
 *                                stays tied to its own generation: an idle
 *                                re-poll never erases or reassigns it.
 */
static volatile uint32_t s_idle_generation = 0;
static volatile bool s_idle_aborted = false;
static volatile uint32_t s_idle_aborted_generation = 0;

/* Explicit manual-session completion, published by the executor (single writer).
 * When a MoveAxis motion drains gracefully, or a MoveAxis command is discarded,
 * its session identity is recorded here together with the drain outcome.  The
 * manual finalization waits on `s_finished_manual_session >= session`, so an
 * unrelated idle barrier or a setpoint-version preemption can never confirm it. */
static volatile uint32_t s_finished_manual_session = 0;
static volatile bool s_finished_manual_aborted = false;

/* --------------------------------------------------------------------------
 * Motion state — active command being executed by the task.
 *
 * `active` and `graceful` are written by motors_motion_stop() from any task
 * (and read by the motion task); every other field is single-writer (motion
 * task only).
 * -------------------------------------------------------------------------- */
static struct {
    volatile bool active;      /* false → abort at next iteration (written by any task) */
    bool aborted;              /* true → loop exited via fault/preempt, not completion */
    volatile bool graceful;    /* true → STOP requested; drain in-flight batches (any task) */
    bool completed;            /* true → a motion ended and READY must be published at the idle barrier */
    bool ran;                  /* true → a motion actually ran this iteration (for the abort capture) */
    /* Stop generation captured at the moment this motion's drain finalised (at
     * the end of motion_loop, not at the idle barrier).  This is the well-defined
     * association point between the drain result and its identity: the result is
     * tagged with the generation current when the motion actually ended, so a
     * later preemption before the barrier, or a later STOP advancing the
     * generation, can never re-tag it.  Single-writer (motion task only). */
    uint32_t drain_generation;
    /* The finalisation motive, captured alongside drain_generation at the end of
     * motion_loop.  `graceful` is writable from any task (motors_motion_stop) and
     * `aborted` reflects the exit path; a STOP that lands *after* the motion ended
     * but before the barrier must not change the published reason, so both are
     * frozen here at the true finalisation point and read back at the barrier. */
    bool drain_graceful;
    bool drain_aborted;
    /* True when the current MoveAxis motion was preempted by a newer setpoint
     * within the same session.  Unlike a STOP (graceful), a preemption drains the
     * committed batches WITHOUT finalising the session: the drain is reused but the
     * manual-session completion is not published.  Single-writer (motion task only). */
    bool preempted;
    volatile MotionCommandType active_cmd_type;  /* volatile: also read by the manual-drain wait */
    int64_t ra_target;         /* steps */
    int64_t dec_target;        /* steps */
    int64_t ra_start;          /* steps — captured at motion start (for ramps) */
    int64_t dec_start;         /* steps */
    /* Tracking mode to restore after the current slew completes (SLEW only). */
    TrackingMode resume_tracking;
    /* MoveAxis setpoint version captured when the current move started — a
     * version bump means the client published a newer rate. */
    uint32_t move_axis_version;
    /* Manual-session identity of the active MoveAxis motion — lets the task
     * detect that its session was finalised/replaced and drain gracefully. */
    uint32_t move_axis_session;

    /* PulseGuide state — single-writer (motion task only). */
    int64_t ra_guide_deadline_us;
    int64_t dec_guide_deadline_us;
    float ra_guide_offset_dps;
    float dec_guide_offset_dps;
} s_motion;

/* --------------------------------------------------------------------------
 * Slew acceleration / deceleration
 * -------------------------------------------------------------------------- */

/*
 * The ramp profile lives in main/motion/slew_ramp.c (shared with the
 * arrival-time estimator).  Convert the executor's step/centidegree state to
 * degrees and delegate to the shared velocity function.
 */
static float slew_velocity_dps(float speed_dps, int64_t travelled_steps,
                               int64_t distance_steps, int distance_cds) {
    float distance_deg = (float) distance_cds / 100.0f;
    float travelled_deg = (distance_steps > 0)
        ? (float) travelled_steps * distance_deg / (float) distance_steps
        : 0.0f;
    return slew_ramp_velocity(&mount_profile.slew, distance_deg, travelled_deg, speed_dps);
}

/* --------------------------------------------------------------------------
 * Step period helpers
 * -------------------------------------------------------------------------- */

/*
 * Convert an angular speed (deg/s) to a step period in RMT ticks.
 * `spp` is the canonical-steps-per-pulse factor captured once at motion start,
 * so the period is derived from the resolution the motion is actually emitted
 * at — never re-read from the mutable TMC state, which a concurrent resolution
 * change could alter mid-motion.  Returns MAX_STEP_PERIOD_TICKS when velocity is
 * effectively zero.
 */
static uint32_t step_period_ticks(float velocity_dps, uint32_t spp) {
    if (fabsf(velocity_dps) < 1e-9f)
        return MAX_STEP_PERIOD_TICKS;

    /* Period of one STEP pulse: canonical degrees per step × the pulse
     * multiplier of this motion's microstep resolution. */
    float deg_per_pulse = motors_get_deg_per_microstep() * (float) spp;
    float period_s = deg_per_pulse / fabsf(velocity_dps);
    uint32_t ticks = (uint32_t) (period_s * (float) RMT_RESOLUTION_HZ);
    if (ticks < STEP_MIN_PERIOD_TICKS)
        ticks = STEP_MIN_PERIOD_TICKS;
    return ticks;
}

/* Clear any in-flight guide pulse so a stale deadline/offset cannot "resume"
 * the pulse after a quick stop → restart or a hardware fault. */
static void clear_guide_state(void) {
    s_motion.ra_guide_deadline_us = 0;
    s_motion.ra_guide_offset_dps = 0.0f;
    s_motion.dec_guide_deadline_us = 0;
    s_motion.dec_guide_offset_dps = 0.0f;
}

/* --------------------------------------------------------------------------
 * Stop the active motion loop from outside the motion task.
 * Sets active=false (abort the loop) and graceful=true (request the in-flight
 * batches to be drained exactly) and notifies the task so it wakes immediately.
 *
 * Safe to call from any task.
 * -------------------------------------------------------------------------- */
void motors_motion_stop(void) {
    /* The stop generation is advanced separately by motors_stop_begin() so that
     * admission-close and generation-bump are one atomic step.  This function
     * only aborts the running motion loop and wakes the task.  The flags are
     * written under the admission lock — the same mechanism the motion task uses
     * to read them at motion start — so the abort is coherent across cores. */
    portENTER_CRITICAL(&motors_admission_lock);
    motors_motion_mark_stopped_locked();
    portEXIT_CRITICAL(&motors_admission_lock);

    clear_guide_state();

    if (motors_motion_task_handle) {
        /* eIncrement (xTaskNotifyGive) — the tracking loop sleeps in
         * ulTaskNotifyTake(pdTRUE, …), which only wakes early when the
         * notification count is non-zero.  eNoAction leaves the count at zero,
         * so it produces a spurious wakeup that re-blocks and delays the stop
         * by up to the sleep timeout. */
        xTaskNotifyGive(motors_motion_task_handle);
    }
}

/*
 * Mark the running motion as gracefully stopped, called with
 * motors_admission_lock already held.  The STOP path holds that lock to bump the
 * stop generation and set this flag in ONE coherent critical section, so the
 * finalisation capture (which reads generation and graceful under the same
 * lock) can never observe a new generation with a stale graceful flag.
 */
void motors_motion_mark_stopped_locked(void) {
    s_motion.active = false;
    s_motion.graceful = true;
}

/* --------------------------------------------------------------------------
 * Motion helpers — small reusable blocks extracted from the hot paths.
 * -------------------------------------------------------------------------- */

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

/*
 * Clamp a batch so the axis never emits a step past its configured limit.
 * `current` is the absolute position before this batch; `sign` is the travel
 * direction (±1).  Applied per batch so a limit changed while a move is in
 * flight still stops the motion at the new boundary.
 */
static uint32_t clamp_ra_batch(int64_t current, int sign, uint32_t batch, uint32_t spp) {
    int64_t limit = (sign > 0) ? motors_deg_to_steps(motors_state.limits.ra_max)
                               : motors_deg_to_steps(motors_state.limits.ra_min);
    int64_t headroom = (sign > 0) ? (limit - current) : (current - limit);
    /* `current`/limit are canonical steps; `batch` is STEP pulses. */
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

/* Mark the motion finished.  READY is published at the idle barrier (top of the
 * motion loop), not here, so a reader never observes READY while the task is
 * still leaving its loop; a resume-tracking transition that follows this sets
 * TRACKING before the barrier and must not be clobbered. */
static void finish_motion(void) {
    s_motion.active = false;
    s_motion.completed = true;
}

/*
 * RMT emission failure — a genuine hardware fault, not a user STOP or a limit.
 * Enter the unrecoverable ERROR state and latch the fault so it is observable
 * through the state snapshot, survives a power-loss recovery, and is
 * distinguishable from a normal pulse completion.  Clear the guide state so a
 * stale deadline cannot resume the pulse, and flag the motion as aborted so the
 * loop tears down the RMT channels on exit.
 */
static void abort_rmt_hardware_fault(void) {
    s_motion.active = false;
    s_motion.aborted = true;
    clear_guide_state();
    motors_enter_hardware_fault();
}

/* Recompute RA / DEC step-period ticks from current ramp or constant speed. */
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

/*
 * Extend or replace a PulseGuide deadline for one axis.
 * Same direction extends, opposite direction replaces.
 */
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

/*
 * Single-step wait during tracking: worst-case 500 ms, blocked in 50 ms slices
 * so an urgent fault (ERROR) interrupts the wait without busy-waiting.  The
 * slice take returns the moment the completion ISR gives the token, so a normal
 * step stays latency-free; only the absent-completion worst case is re-polled.
 *
 * A single tracking pulse is one RMT symbol (STEP_PULSE_TICKS + STEP_MIN_LOW_TICKS
 * = 4 us), so its completion arrives in microseconds or never; 500 ms is a pure
 * hang-detection bound.  It is deliberately <= MOTION_IDLE_TIMEOUT_US (1 s) so a
 * STOP's rest confirmation always outlasts this wait: the executor faults (or
 * reconciles) before the producer's idle wait can expire.
 */
#define TRACKING_STEP_TIMEOUT_US 500000
#define TRACKING_STEP_SLICE_MS   50

/*
 * Wait for one tracking step's completion, interrupting early on an urgent
 * fault.  Reuses motors_status_is_error — the same predicate
 * check_motion_conditions uses — so the interrupt condition is identical to the
 * loop-top abort.  A normal STOP (active=false, graceful) is NOT treated as an
 * interrupt: the committed pulse's completion is allowed to arrive and reconcile
 * within the deadline, and the caller accounts it before checking s_motion.active.
 * Returns ESP_OK on a confirmed step, ESP_ERR_TIMEOUT on a fault or expired
 * deadline (the step is indeterminate).
 */
static esp_err_t wait_tracking_step_done(esp_err_t (*wait)(TickType_t)) {
    const int64_t deadline_us = esp_timer_get_time() + TRACKING_STEP_TIMEOUT_US;
    while (true) {
        esp_err_t e = wait(pdMS_TO_TICKS(TRACKING_STEP_SLICE_MS));
        if (e == ESP_OK) return ESP_OK;
        if (motors_status_is_error(motors_state.status)) return ESP_ERR_TIMEOUT;
        if (esp_timer_get_time() > deadline_us) return ESP_ERR_TIMEOUT;
    }
}

/*
 * A tracking step was submitted to RMT but its completion never arrived: the
 * pulse may or may not have been physically emitted, so the position is no
 * longer trustworthy until a re-reference (set_current_as_home) clears it.
 */
static void mark_tracking_position_uncertain(void) {
    portENTER_CRITICAL(&motors_state_lock);
    motors_state.position_uncertain = true;
    portEXIT_CRITICAL(&motors_state_lock);
}

/*
 * Emit a single RA tracking step in the given direction.
 * Returns true on success, false if motion must abort (limit hit, RMT error).
 */
static bool emit_ra_tracking_step(int direction, rmt_symbol_word_t *buffer) {
    int64_t next = motors_state.ra_steps + (int64_t)direction;

    if (!motors_is_valid_ra_steps(next)) {
        ESP_LOGW(TAG, "RA limit at %.3f deg",
                 (double)motors_steps_to_deg(motors_state.ra_steps));
        s_motion.active = false;
        /* A limit hit ends the tracking base AND any in-flight guide on either
         * axis.  Clear both deadlines so the post-loop guiding recompute in
         * motors_motion_task_run (ra_active/dec_active) sees them expired;
         * otherwise it would recompute guiding=true and leave it stuck until
         * the next command.  Flag truncation if a guide was actually cut short,
         * regardless of which axis carried it. */
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
        /* The pulse was submitted but its completion was lost: indeterminate. */
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

/* --------------------------------------------------------------------------
 * Motion conditions check — returns false when the current motion should end.
 * -------------------------------------------------------------------------- */
static bool check_motion_conditions(void) {
    /*
     * Motor ERROR — immediate abort of any active motion.
     *
     * If the motors subsystem enters ERROR state while slewing or
     * tracking, kill RMT output and physically disable the drivers
     * on the spot.  This is the emergency cut-off: no deceleration,
     * no position cleanup — just stop NOW.
     */
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

    /*
     * Slew / move-axis completion: both axes at target.
     *
     * Use active_cmd_type rather than motors_state.status because the
     * caller may have already set status to TRACKING (resume-after-slew
     * pattern in motors_slew_to_angle / motors_slew_axis_*) before the
     * motion task reaches the target.  Checking the authoritative
     * s_motion field guarantees completion is always detected.
     */
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

/* --------------------------------------------------------------------------
 * Command processing — handle one MotionCommand and set up motion state.
 *
 * Only motion-producing commands (SLEW, TRACK, MOVE_AXIS) go through
 * the queue.  Stop / park are handled directly by their
 * callers via motors_motion_stop() + motors_state update.
 * -------------------------------------------------------------------------- */

/*
 * Enter tracking for `mode`: set the live base rate and the open-ended RA
 * target so the tracking loop runs until an external stop.  Shared by the TRACK
 * command and the inline tracking resume after a slew — the resume is therefore
 * atomic with the slew, not a second queue order that could be lost.
 */
static void begin_tracking(TrackingMode mode) {
    float ra_speed = motors_get_tracking_speed(mode);

    portENTER_CRITICAL(&motors_state_lock);
    motors_state.ra_speed = ra_speed;
    motors_state.dec_speed = 0.0f;
    motors_state.status = MOTORS_STATUS_TRACKING;
    motors_state.tracking = mode;
    portEXIT_CRITICAL(&motors_state_lock);

    /*
     * Tracking runs open-ended: target is set to the axis limit so the loop
     * never completes on its own — it only stops when an external status
     * change (STOP, PARK) is detected.  RA always tracks positive (hour angle
     * increases at the sidereal rate on both hemispheres), so the target is
     * ra_max; ra_min is the fallback for a negative rate.
     */
    s_motion.ra_target = (ra_speed >= 0.0f)
        ? motors_deg_to_steps(motors_state.limits.ra_max)
        : motors_deg_to_steps(motors_state.limits.ra_min);
    s_motion.dec_target = motors_state.dec_steps;
    s_motion.ra_start = motors_state.ra_steps;
    s_motion.dec_start = motors_state.dec_steps;
    s_motion.active_cmd_type = MOTION_CMD_TRACK;
    s_motion.active = true;
}

static void process_command(MotionCommand cmd) {
    /*
     * If motors entered ERROR while this command was queued, reject it
     * without touching state.  The mount layer already guards against
     * enqueuing commands in ERROR, but a race between enqueue and
     * error-detection could still deliver a stale command here.
     */
    if (motors_status_is_error(motors_state.status)) {
        return;
    }

    /* Discard a command invalidated by a STOP that landed after it was enqueued
     * but before it was dequeued here.  Read the generation under the admission
     * lock — the same mechanism the STOP uses to bump it — so the comparison is
     * coherent with a concurrent stop. */
    if (cmd.generation != motors_stop_generation_get()) {
        return;
    }

    s_motion.active_cmd_type = cmd.type;
    s_motion.resume_tracking = TRACKING_NONE;

    switch (cmd.type) {
        case MOTION_CMD_SLEW:
            portENTER_CRITICAL(&motors_state_lock);
            motors_state.ra_speed = cmd.ra_speed;
            motors_state.dec_speed = cmd.dec_speed;
            motors_state.status = MOTORS_STATUS_SLEWING;
            motors_state.tracking = TRACKING_NONE;
            portEXIT_CRITICAL(&motors_state_lock);

            if (cmd.relative) {
                s_motion.ra_target = motors_state.ra_steps
                                     + motors_deg_to_steps(cmd.ra_delta_deg);
                s_motion.dec_target = motors_state.dec_steps
                                      + motors_deg_to_steps(cmd.dec_delta_deg);
            } else {
                s_motion.ra_target = motors_deg_to_steps(cmd.ra_target_deg);
                s_motion.dec_target = motors_deg_to_steps(cmd.dec_target_deg);
            }
            /* Re-clamp at execution: the relative destination is computed from
             * the live position, which may have advanced since the request was
             * validated (queued moves).  A mechanical limit must hold even when
             * two valid moves stack past it. */
            s_motion.ra_target = clamp_ra_steps(s_motion.ra_target);
            s_motion.dec_target = clamp_dec_steps(s_motion.dec_target);
            s_motion.ra_start = motors_state.ra_steps;
            s_motion.dec_start = motors_state.dec_steps;
            s_motion.resume_tracking = cmd.resume_tracking;
            s_motion.active = true;

            break;

        case MOTION_CMD_TRACK:
            begin_tracking(cmd.tracking_mode);
            break;

        case MOTION_CMD_MOVE_AXIS:
            /* MoveAxis is a setpoint: read the newest published rate, not the
             * (possibly stale) queued value, so obsolete orders never win. */
            {
                float ra_sp, dec_sp;
                uint32_t ver;

                /* Discard a MoveAxis whose manual session no longer matches the
                 * active one: either it was finalised (manual_move false) or a
                 * newer session replaced it (identity changed).  A global STOP
                 * generation no longer distinguishes two manual sessions. */
                bool session_active;
                uint32_t cur_session;
                portENTER_CRITICAL(&motors_admission_lock);
                session_active = motors_manual_move;
                cur_session = motors_manual_session;
                portEXIT_CRITICAL(&motors_admission_lock);
                if (!session_active || cmd.session != cur_session) {
                    /* A discarded MoveAxis marks its session as finished: it can
                     * never begin emitting pulses, so a finalization of that
                     * session may complete.  When the discarded command belongs
                     * to the last session (no newer one active), a preceding
                     * same-session motion may have aborted via a setpoint-version
                     * preempt and left the mount SLEWING; publish READY here so a
                     * finalization never returns while the mount is stuck. */
                    portENTER_CRITICAL(&motors_state_lock);
                    if (cmd.session > s_finished_manual_session) {
                        s_finished_manual_session = cmd.session;
                        s_finished_manual_aborted = false;
                    }
                    if (!session_active
                        && motors_state.status == MOTORS_STATUS_SLEWING) {
                        motors_state.status = MOTORS_STATUS_READY;
                    }
                    portEXIT_CRITICAL(&motors_state_lock);
                    return;
                }

                /* The setpoint tuple is guarded by motors_admission_lock (the
                 * producer publishes it in the same section as the enqueue). */
                portENTER_CRITICAL(&motors_admission_lock);
                ra_sp = motors_move_axis_ra_dps;
                dec_sp = motors_move_axis_dec_dps;
                ver = motors_move_axis_version;
                portEXIT_CRITICAL(&motors_admission_lock);

                /* Capture the tracking mode to restore when the manual session
                 * ends.  This is the transition point: the tracking loop just
                 * ended, so the mode read here is the one actually active — not
                 * a pre-admission snapshot a STOP may have invalidated. */
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
                s_motion.move_axis_session = cmd.session;
                s_motion.active = true;

                /* Test hook: pause after the tracking mode is captured and the
                 * state is set to SLEWING, but before saved_tracking is published
                 * — so a test can exercise a STOP that invalidates the session in
                 * that window (E03-D). */
                motors_test_hook_before_move_axis_saved_tracking_publish();

                /* Record the saved tracking only when transitioning from
                 * tracking AND the session is still the one being processed (a
                 * STOP may have invalidated it in the window above, and a newer
                 * session may have replaced it — the boolean alone cannot tell
                 * the two apart).  The check and the publication are one
                 * admission-lock critical section, so a STOP or a newer session
                 * cannot invalidate between them. */
                if (saved != TRACKING_NONE) {
                    portENTER_CRITICAL(&motors_admission_lock);
                    if (motors_manual_move
                        && motors_manual_session == cmd.session) {
                        motors_saved_tracking = saved;
                    }
                    portEXIT_CRITICAL(&motors_admission_lock);
                }
            }

            break;

        case MOTION_CMD_PULSE_GUIDE:
            if (cmd.guide_axis == 0) {
                update_guide(&s_motion.ra_guide_deadline_us,
                             &s_motion.ra_guide_offset_dps, &cmd);
            } else {
                update_guide(&s_motion.dec_guide_deadline_us,
                             &s_motion.dec_guide_offset_dps, &cmd);
            }

            motors_state.guiding = true;
            /* A fresh guide clears the previous truncation latch. */
            motors_state.guide_truncated = false;
            /*
             * Set active so a standalone guide pulse (READY, no tracking)
             * actually runs its motion loop.  During tracking active is
             * already true, so this is a no-op there.
             */
            s_motion.active = true;
            break;
    }
}

/* --------------------------------------------------------------------------
 * Slewing & move-axis motion loop — RMT batch scheduling.
 *
 * Replaces software busy-wait with DMA-driven step generation:
 *
 *   1. Determine direction (constant for the entire slew) and set DIR pins.
 *   2. Every ~5 ms recalculate velocity via the ramp curve.
 *   3. Every ~20 ms compute a batch of steps at the current velocity,
 *      encode them as RMT symbols, and transmit via DMA.
 *   4. Block on a semaphore while the RMT peripheral drives the step
 *      timing — near-zero CPU; the pulse shape is exact.
 *   5. On wake-up, update positions by the full batch count and loop.
 *
 * MOVE_AXIS (joystick / NINA centering) skips the ramp — constant
 * velocity from the first microstep so the client's time × rate
 * distance calculations are accurate.
 * -------------------------------------------------------------------------- */
/*
 * Encode one axis batch — shared between the initial batch and pre-encoded
 * batches inside the double-buffered slewing loop.  Preserves the exact same
 * encoding logic (MOVE_AXIS constant-period vs SLEW per-step ramp).
 */
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

/*
 * Correct the coarse-microstep residual after a slew: at the coarse slew
 * resolution one STEP pulse advances `spp` canonical steps, so up to spp-1 canonical steps may
 * remain short of the target.  Switch back to fine resolution and emit one
 * pulse per remaining canonical step.  Returns false only on abort/error.
 */
static bool slew_residual_correction(int64_t ra_dist, int64_t dec_dist,
                                     uint32_t total_ra_steps, uint32_t total_dec_steps,
                                     int ra_sign, int dec_sign, uint32_t spp)
{
    int ra_residual = (int)(ra_dist - (int64_t)total_ra_steps * (int64_t)spp);
    int dec_residual = (int)(dec_dist - (int64_t)total_dec_steps * (int64_t)spp);

    if (ra_residual == 0 && dec_residual == 0) {
        return true;   /* already exact */
    }

    /* Fine resolution: one STEP pulse == one canonical step. */
    if (!motors_microsteps_prepare(MOTION_CMD_TRACK, 0, 0)) {
        return false;   /* mount is in ERROR */
    }

    rmt_symbol_word_t sym;

    for (int i = 0; i < ra_residual; i++) {
        if (!s_motion.active) return false;
        int64_t next = motors_state.ra_steps + (int64_t)ra_sign;
        if (!motors_is_valid_ra_steps(next)) break;   /* limit — accept the residual */

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
        if (!motors_is_valid_dec_steps(next)) break;   /* limit — accept the residual */

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

/*
 * Double-buffered slewing / move-axis motion loop.
 *
 * Per-axis ping-pong strategy:
 *   buf[ping]  — currently transmitting via RMT+DMA
 *   buf[pong]  — queued in the TX FIFO behind ping
 *
 *   First:  encode → transmit (with semaphore drain) for both axes
 *   Loop:   advance each axis independently as its ping completes —
 *           confirm → encode → transmit (no drain) → swap that axis's
 *           ping/pong, without waiting for the other axis
 *
 * This eliminates both the software encoding gap (the next batch is
 * already queued and starts with zero gap) and the coupling between
 * axes: a slow axis no longer stalls a fast one.
 */
/* Account the two submitted batches (ping + pong) for one axis, each exactly
 * once, waiting for their RMT completions within a shared deadline.  Returns
 * false when the deadline expires with a batch still unconfirmed — the caller
 * then aborts and marks the position uncertain. */
static bool drain_axis_batches(uint32_t batch[2], int ping, int pong,
                               int sign, uint32_t spp, int64_t *steps,
                               bool (*try_wait)(void), int64_t deadline_us) {
    while (batch[ping] > 0 || batch[pong] > 0) {
        /* Promote a queued pong into the free in-flight slot. */
        if (batch[ping] == 0 && batch[pong] > 0) {
            batch[ping] = batch[pong];
            batch[pong] = 0;
        }
        if (batch[ping] > 0 && try_wait()) {
            /* A completion is already available — consume and account it even if
             * the deadline has passed, so a completed batch on this axis is never
             * lost because the *other* axis (or an earlier wait) exhausted the
             * shared deadline.  This also preserves known work during an abort. */
            portENTER_CRITICAL(&motors_state_lock);
            *steps += (int64_t) sign * (int64_t) batch[ping] * (int64_t) spp;
            portEXIT_CRITICAL(&motors_state_lock);
            batch[ping] = batch[pong];
            batch[pong] = 0;
        } else if (batch[ping] > 0) {
            motors_test_hook_drain_wait();
            /* No completion yet — an urgent fault interrupts the wait rather than
             * blocking on a transmission that is unsafe to continue. */
            if (motors_status_is_error(motors_state.status)) {
                return false;
            }
            /* Only now is the deadline the boundary. */
            if (esp_timer_get_time() > deadline_us) {
                return false;
            }
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
    return true;
}

static void slewing_loop_rmt(void) {
    /* Total step counts — exact from int64_t targets. */
    int64_t ra_dist = s_motion.ra_target - s_motion.ra_start;
    int64_t dec_dist = s_motion.dec_target - s_motion.dec_start;
    if (ra_dist < 0) ra_dist = -ra_dist;
    if (dec_dist < 0) dec_dist = -dec_dist;

    /* The RMT loop schedules STEP pulses.  At the coarse microstep resolution
     * one pulse advances the axis by `spp` canonical steps, so total/remaining/
     * done are all kept in PULSES here; only the motors_state counter and the
     * limit headroom (in clamp_*_batch) use canonical units. */
    uint32_t spp = motors_steps_per_pulse();
    uint32_t total_ra_steps = (uint32_t) (ra_dist / (int64_t) spp);
    uint32_t total_dec_steps = (uint32_t) (dec_dist / (int64_t) spp);
    uint32_t ra_steps_done = 0;
    uint32_t dec_steps_done = 0;

    /* Distances in centidegrees for ramp threshold comparisons. */
    int distance_ra_cds = (int)(motors_steps_to_deg(ra_dist) * 100.0f);
    int distance_dec_cds = (int)(motors_steps_to_deg(dec_dist) * 100.0f);

    /*
     * Direction is constant for a slew — set DIR pins once.
     */
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

    /*
     * Double-buffer per axis: ping = in-flight, pong = queued behind it.
     * Each axis advances independently, so a slow axis never stalls the
     * other.  Stack-allocated, DMA-safe on ESP32-S3.
     */
    rmt_symbol_word_t ra_buf[2][RMT_BUFFER_SYMBOLS];
    rmt_symbol_word_t dec_buf[2][RMT_BUFFER_SYMBOLS];
    uint32_t ra_num[2] = {0, 0};
    uint32_t dec_num[2] = {0, 0};
    uint32_t ra_batch[2] = {0, 0};
    uint32_t dec_batch[2] = {0, 0};
    int ra_ping = 0;   /* RA: currently transmitting */
    int ra_pong = 1;   /* RA: queued behind ping */
    int dec_ping = 0;  /* DEC: currently transmitting */
    int dec_pong = 1;  /* DEC: queued behind ping */

    uint32_t ra_period = MAX_STEP_PERIOD_TICKS;
    uint32_t dec_period = MAX_STEP_PERIOD_TICKS;
    int64_t last_ramp_recalc_us = 0;
    bool is_move_axis = (s_motion.active_cmd_type == MOTION_CMD_MOVE_AXIS);

    /* ── Compute + encode + submit first batch (buf[0]) ── */

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

    /* Encode first batch. */
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

    /* Check stop before touching hardware. */
    if (!s_motion.active) return;

    /* Submit first batch (with semaphore drain). */
    esp_err_t tx_err = ESP_OK;
    if (ra_batch[0] > 0) tx_err = motors_rmt_transmit_ra(ra_buf[0], ra_num[0]);
    if (dec_batch[0] > 0 && tx_err == ESP_OK)
        tx_err = motors_rmt_transmit_dec(dec_buf[0], dec_num[0]);
    if (tx_err != ESP_OK) {
        ESP_LOGE(TAG, "RMT transmit failed: %s", esp_err_to_name(tx_err));
        abort_rmt_hardware_fault();
        return;
    }

    /*
     * Pre-encode AND pre-submit the second batch (buf[1]) while buf[0]
     * is transmitting.  This is the key: by the time buf[0] finishes,
     * buf[1] is already queued in the RMT TX FIFO and starts with
     * zero CPU-dependent gap.
     */
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

        /* Submit buf[1] while buf[0] is still transmitting — ZERO GAP */
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

    /* ── Main decoupled loop — each axis advances independently ──
     *
     * Invariant at loop entry:
     *   ra_buf[ra_ping] / dec_buf[dec_ping] — submitted, transmitting
     *   ra_buf[ra_pong] / dec_buf[dec_pong] — submitted, queued in TX FIFO
     *
     * Each iteration:
     *   1. recompute ramp periods (shared, throttled to 5 ms)
     *   2. advance an axis as soon as its in-flight batch completes —
     *      confirm position, encode + submit its next batch, swap its
     *      ping/pong — without waiting for the other axis
     *   3. check stop / conditions / finish
     *   4. yield briefly to avoid a busy-wait
     */

    while (s_motion.active) {
        motors_test_hook_loop_top();
        /* Read the shared MoveAxis state (setpoint version, manual-session
         * identity and validity) under the admission lock — the same lock the
         * producer and the finalization use to write them — so the checks below
         * are coherent rather than racy `volatile` reads. */
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

        /* The manual session that owns this motion was finalised or replaced:
         * drain gracefully (not abort) so the committed batches are accounted
         * and the session's own completion can be confirmed.  This must be
         * checked before the setpoint-version preempt below: a replacement
         * session bumps the version too, and mistaking it for a plain rate
         * change would abort the old session (dropping its in-flight batches)
         * instead of draining it. */
        if (is_move_axis && (!manual || session != s_motion.move_axis_session)) {
            s_motion.active = false;
            s_motion.graceful = true;
            break;
        }

        /* A newer MoveAxis setpoint within the SAME session preempts the running
         * one: reconcile the committed batches (drain) and let the main loop
         * re-read the setpoint and apply it, without finalising the session.
         * Uses the version counter rather than peeking the queue, so a
         * non-MOVE_AXIS command interleaved ahead of a newer rate cannot hide
         * it. */
        if (is_move_axis && setpoint_version != s_motion.move_axis_version) {
            s_motion.active = false;
            s_motion.preempted = true;
            break;
        }

        /*
         * 1. Ramp recalc — pick the next batch size from each axis's
         *    projected travelled (pong batch included).
         */
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

        /* 2a. RA — advance when its in-flight batch completes. */
        if (ra_steps_done < total_ra_steps
            && ra_batch[ra_ping] > 0
            && motors_rmt_try_wait_ra()) {
            portENTER_CRITICAL(&motors_state_lock);
            motors_state.ra_steps += (int64_t)ra_sign * (int64_t)ra_batch[ra_ping] * (int64_t)spp;
            portEXIT_CRITICAL(&motors_state_lock);
            ra_steps_done += ra_batch[ra_ping];
            ra_batch[ra_ping] = 0;   /* ping accounted — slot now free */
            if (!s_motion.active) break;   /* graceful: don't submit the next */

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

        /* 2b. DEC — advance when its in-flight batch completes. */
        if (dec_steps_done < total_dec_steps
            && dec_batch[dec_ping] > 0
            && motors_rmt_try_wait_dec()) {
            portENTER_CRITICAL(&motors_state_lock);
            motors_state.dec_steps += (int64_t)dec_sign * (int64_t)dec_batch[dec_ping] * (int64_t)spp;
            portEXIT_CRITICAL(&motors_state_lock);
            dec_steps_done += dec_batch[dec_ping];
            dec_batch[dec_ping] = 0;   /* ping accounted — slot now free */
            if (!s_motion.active) break;   /* graceful: don't submit the next */

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

        /* 3. Stop / conditions after any position updates. */
        if (!check_motion_conditions()) break;
        if (!s_motion.active) break;

        /* Target reached on both axes? */
        if (ra_steps_done >= total_ra_steps && dec_steps_done >= total_dec_steps) {
            if (slew_residual_correction(ra_dist, dec_dist, total_ra_steps, total_dec_steps,
                                         ra_sign, dec_sign, spp)) {
                finish_motion();
            }
            break;
        }

        /* 4. Yield — batch durations are far coarser than 1 ms, so this
         *    adds no meaningful latency and keeps the CPU free. */
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    /* Graceful STOP or a setpoint preemption: account the in-flight + queued
     * batches already submitted to RMT, so the position stays exact.  Both axes
     * share one deadline bounded by the maximum committed transmission (see
     * MOTION_DRAIN_TIMEOUT_US).  A drain that cannot complete degrades to an
     * abort.  A preemption drains here without finalising the session; the
     * barrier publishes the session completion only for a graceful stop. */
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
        /* Urgent abort: consume the completions that are ALREADY available, without
         * waiting, so a batch that fully completed before the fault keeps its known
         * canonical displacement — the RMT channels are about to be reset and their
         * pending completions discarded.  Unknown (partially-emitted) batches are
         * left unconsumed and surface as position_uncertain.  `deadline_us == 0`
         * makes drain_axis_batches non-blocking: it consumes available completions
         * in FIFO order and returns on the first unavailable one. */
        (void) drain_axis_batches(ra_batch, ra_ping, ra_pong, ra_sign, spp,
                                  &motors_state.ra_steps, motors_rmt_try_wait_ra,
                                  0);
        (void) drain_axis_batches(dec_batch, dec_ping, dec_pong, dec_sign, spp,
                                  &motors_state.dec_steps, motors_rmt_try_wait_dec,
                                  0);
    }
}

/* --------------------------------------------------------------------------
 * Tracking motion loop — absolute-time fractional accumulator + RMT pulse.
 *
 * Designed for continuous open-ended tracking (sidereal, solar, lunar)
 * where timing precision must hold over arbitrarily long sessions.
 *
 * Scheduling strategy (hybrid sleep + fine-wait → RMT):
 *
 *   dt = now - last_time
 *   accumulator += dt / period_us     (period_us converted from RMT ticks)
 *   while (accumulator >= 1.0):  encode 1 step, rmt_transmit, wait, acc -= 1.0
 *
 *   deadline = now + (1.0 - accumulator) * period_us    (µs-exact)
 *   if deadline - now > 2 ms:
 *       vTaskDelay most of it (capped 50 ms, yields CPU → near-zero consumption)
 *   fine-wait remaining margin with busy-wait → µs precision
 *
 * The actual STEP pulse is generated by the RMT peripheral (hardware-timed
 * shape).  The fine-wait determines *when* the pulse begins; the RMT
 * determines the pulse *shape*.  Long idle periods (tracking ≈ 22 ms at
 * sidereal rate) are streamed as idle-only RMT symbols via DMA — the CPU
 * sleeps through the entire step period.
 *
 * Only RA is stepped during tracking; DEC velocity is always zero.
 * -------------------------------------------------------------------------- */
/*
 * Recompute the effective velocity for one axis, folding in any active
 * PulseGuide offset.  Returns the RMT step period in ticks.
 */
static void tracking_loop_rmt(void) {
    const int64_t FINE_MARGIN_US = 2000;
    const float deg_per_step = motors_get_deg_per_microstep();

    /* Signed phase accumulators in microsteps. */
    double ra_phase = 0.0;
    double dec_phase = 0.0;
    int64_t last_time_us = esp_timer_get_time();
    int64_t last_check_us = last_time_us;

    int ra_dir_set = 0;   /* 0 = unknown, ±1 = last set RA direction */
    int dec_dir_set = 0;
    rmt_symbol_word_t ra_sym[RMT_BUFFER_SYMBOLS];
    rmt_symbol_word_t dec_sym[RMT_BUFFER_SYMBOLS];

    /*
     * Effective velocities (deg/s).  Persist across iterations so the
     * phase integral at the top of the loop uses the velocity that was
     * actually in effect during the elapsed interval, before any guide
     * start/end events at `now` are applied.
     */
    float ra_vel = (motors_state.status == MOTORS_STATUS_TRACKING)
                   ? motors_state.ra_speed : 0.0f;
    float dec_vel = 0.0f;

    while (s_motion.active) {
        int64_t now = esp_timer_get_time();
        double dt_s = (double)(now - last_time_us) / 1000000.0;
        last_time_us = now;

        /*
         * Integrate the interval that just elapsed using the velocities
         * that were in effect during it (ra_vel/dec_vel hold the previous
         * iteration's values).  Guide start/end events at `now` are applied
         * after this, so each velocity covers exactly its own interval —
         * no retroactive guide motion.
         */
        ra_phase += (double)ra_vel * dt_s / (double)deg_per_step;
        dec_phase += (double)dec_vel * dt_s / (double)deg_per_step;

        /*
         * Drain queued guiding and tracking-mode updates while this loop is
         * active.  The task never returns to xQueueReceive() in the main loop
         * during tracking, so PHD2 corrections and tracking-mode changes would
         * otherwise sit unprocessed — and a pending TRACK at the front of the
         * queue would block the PulseGuide corrections queued behind it.
         * TRACK is idempotent here (re-applying the same mode is a no-op).
         * SLEW and MOVE_AXIS must not run inline: they stay queued and are
         * preceded by a STOP that ends this loop anyway.
         */
        {
            MotionCommand queued;
            while (xQueuePeek(motion_cmd_queue, &queued, 0) == pdTRUE
                   && (queued.type == MOTION_CMD_PULSE_GUIDE
                       || queued.type == MOTION_CMD_TRACK)) {
                xQueueReceive(motion_cmd_queue, &queued, 0);
                process_command(queued);
            }
        }

        /* A MOVE_AXIS accepted while tracking ends this loop so the main loop
         * dequeues and dispatches it (the transition from tracking to manual
         * motion, without a global STOP). */
        {
            MotionCommand queued;
            if (xQueuePeek(motion_cmd_queue, &queued, 0) == pdTRUE
                && queued.type == MOTION_CMD_MOVE_AXIS) {
                s_motion.active = false;
                break;
            }
        }

        /* Guide expiry — checked every iteration for µs-precise pulse
         * termination instead of the previous 10 ms quantization. */
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

        /* Velocities for the interval starting at `now`.  Standalone
         * guiding (status != TRACKING) has a zero base rate; only tracking
         * carries the continuous RA speed. */
        ra_vel = (motors_state.status == MOTORS_STATUS_TRACKING)
                 ? motors_state.ra_speed : 0.0f;
        if (s_motion.ra_guide_deadline_us)
            ra_vel += s_motion.ra_guide_offset_dps;
        dec_vel = 0.0f;
        if (s_motion.dec_guide_deadline_us)
            dec_vel = s_motion.dec_guide_offset_dps;

        /* Conditions check (every ~500 us). */
        if (now - last_check_us >= 500) {
            last_check_us = now;
            if (!check_motion_conditions()) break;
            if (motors_state.status != MOTORS_STATUS_TRACKING
                && !motors_state.guiding) {
                /* Standalone guide finished — clear the motion flag so
                 * idle state is unambiguous. */
                s_motion.active = false;
                break;
            }
        }

        /* ── Emit RA steps ─────────────────────────────────── */
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

        /* ── Emit DEC steps ─────────────────────────────────── */
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
            /* Account the confirmed step even if STOP arrived during the wait. */
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
            /* Account the confirmed step even if STOP arrived during the wait. */
            portENTER_CRITICAL(&motors_state_lock);
            motors_state.dec_steps = next;
            portEXIT_CRITICAL(&motors_state_lock);
            dec_phase += 1.0;
            if (!s_motion.active) { dec_phase = 0.0; return; }
        }

        /* ── Sleep ─────────────────────────────────────────── */
        /* Time to the next step boundary.  The next step is at
         * phase == +1 for positive velocity, at phase == -1 for
         * negative velocity, so the remaining phase depends on the
         * sign of the velocity. */
        double ra_remaining = (ra_vel > 0.0) ? (1.0 - ra_phase)
                                             : (1.0 + ra_phase);
        double dec_remaining = (dec_vel > 0.0) ? (1.0 - dec_phase)
                                               : (1.0 + dec_phase);
        double ra_s = (ra_vel != 0.0) ? ra_remaining * deg_per_step / fabs((double)ra_vel) : 1e9;
        double dec_s = (dec_vel != 0.0) ? dec_remaining * deg_per_step / fabs((double)dec_vel) : 1e9;
        int64_t w_us = (int64_t)((ra_s < dec_s ? ra_s : dec_s) * 1000000.0);
        if (w_us < 0) w_us = 0;

        /*
         * Wake at the earliest guide deadline so a pulse ends exactly on
         * time instead of rounding up to the next step period.  Fresh
         * clock read: step emission above may have consumed time since
         * `now` was captured at loop entry.
         */
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
            /* Block instead of busy-waiting.  At high guide rates the step
             * period (~187 us) is far below the 1 ms tick, so a busy-wait here
             * monopolizes the core and starves lower-priority tasks (the HTTP
             * server) for the whole pulse — PulseGuide responses then stall.
             * Blocking yields the CPU; the phase accumulator already integrates
             * the true elapsed time, so the coarser granularity only adds a
             * little step-timing jitter, never a cumulative rate error. */
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1));
        }
    }
}

/* --------------------------------------------------------------------------
 * Motion loop dispatcher.
 *
 * Routes to the appropriate execution path based on mount status:
 *   - TRACKING (sidereal / solar / lunar) → tracking_loop_rmt()
 *     (absolute-time fractional accumulator, zero cumulative error)
 *   - Everything else (SLEWING, MOVE_AXIS, etc.) → slewing_loop_rmt()
 *     (batched RMT with ramps)
 * -------------------------------------------------------------------------- */
static void motion_loop(void) {
    /* Motor ERROR — refuse to enter any motion loop. */
    if (motors_status_is_error(motors_state.status)) {
        return;
    }

    /* Synchronize the start of the motion with STOP: read `active` and reset
     * `aborted`/`graceful` together under the admission lock — the same lock the
     * STOP uses to write them — so a STOP that landed after process_command()
     * set active=true is seen coherently and its request is not clobbered.
     * `preempted` is single-writer (motion task) and reset here too. */
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

    /* Select the microstep resolution for this motion BEFORE any STEP pulse is
     * emitted.  The microstep module owns the policy (coarse for large ramped
     * slews, fine otherwise).  A failed switch stops the mount and puts it into
     * ERROR — abort here. */
    int64_t ra_dist = 0;
    int64_t dec_dist = 0;
    if (s_motion.active_cmd_type == MOTION_CMD_SLEW) {
        ra_dist = s_motion.ra_target - s_motion.ra_start;
        dec_dist = s_motion.dec_target - s_motion.dec_start;
    }
    if (!motors_microsteps_prepare(s_motion.active_cmd_type, ra_dist, dec_dist)) {
        return;
    }

    /* A motion is about to run: mark it so the idle barrier captures the abort
     * result exactly once (and not on a later idle re-poll or generation bump). */
    s_motion.ran = true;

    /* Standalone guide pulse (no tracking) — use the tracking loop
     * with zero base rate.  It handles DEC-only and RA-only guiding. */
    if ((motors_state.status == MOTORS_STATUS_TRACKING
         && motors_state.tracking != TRACKING_NONE)
        || motors_state.guiding) {
        tracking_loop_rmt();
    } else {
        slewing_loop_rmt();

        /*
         * A slew enqueued while tracking was active carries the mode to
         * restore.  Resume it inline — not as a separate queue order — so the
         * transition is atomic and cannot be lost to a queue-full or a
         * concurrent STOP.  An aborted slew (STOP) cancels the resume too.
         */
        if (!s_motion.aborted && !s_motion.graceful
            && s_motion.resume_tracking != TRACKING_NONE) {
            TrackingMode resume = s_motion.resume_tracking;
            s_motion.resume_tracking = TRACKING_NONE;

            /* A coarse slew left the drivers at the coarse resolution; tracking
             * runs at fine resolution, so restore it before the tracking loop. */
            if (!motors_microsteps_prepare(MOTION_CMD_TRACK, 0, 0)) {
                return;   /* mount is in ERROR */
            }

            begin_tracking(resume);
            tracking_loop_rmt();
        }
    }

    /*
     * Recreate the RMT channels only after an external abort (STOP / PARK /
     * preempt) — the one case where a queued double-buffered batch must be
     * dropped.  On normal completion every batch has finished, so a full
     * teardown + recreate is unnecessary and, done after every single move,
     * glitches the STEP output and can brown-out the board.
     */
    if (s_motion.aborted) {
        motors_rmt_reset_both();

        /*
         * An aborted batched move (STOP / preempt during SLEW or MOVE_AXIS)
         * drops whatever fraction of the in-flight batch had already been
         * emitted but not yet counted, so the step counters may be off by up
         * to one batch (RMT_BATCH_MAX_STEPS microsteps).  Surface that
         * uncertainty so consumers stop trusting the position as absolute
         * until a physical re-reference clears it.  The tracking loop emits
         * one step per transaction and only counts it once it fully completes,
         * so it is excluded here.
         */
        if (s_motion.active_cmd_type == MOTION_CMD_SLEW
            || s_motion.active_cmd_type == MOTION_CMD_MOVE_AXIS) {
            portENTER_CRITICAL(&motors_state_lock);
            motors_state.position_uncertain = true;
            portEXIT_CRITICAL(&motors_state_lock);
        }
    }

    /* The drain result is final here: capture the identity AND the finalisation
     * motive (graceful/aborted) at the true finalisation point, so the idle
     * barrier publishes a stable result independent of when it runs.  A STOP that
     * lands after this point (before the barrier) changes s_motion.graceful, but
     * the published reason stays the one the motion actually ended with.
     *
     * Read the generation and the graceful flag together under the admission
     * lock: a concurrent STOP bumps the generation (motors_stop_begin) and sets
     * graceful (motors_motion_stop) in separate critical sections, so reading
     * them without a shared section could tag the result with a foreign identity
     * (generation read before the STOP, graceful read after it). */
    portENTER_CRITICAL(&motors_admission_lock);
    s_motion.drain_generation = motors_stop_generation;
    s_motion.drain_graceful = s_motion.graceful;
    portEXIT_CRITICAL(&motors_admission_lock);
    s_motion.drain_aborted = s_motion.aborted;
    motors_test_hook_after_motion_loop();
}

/* --------------------------------------------------------------------------
 * Motion task entry point.
 *
 * Blocks on the command queue when idle. When a motion-producing command
 * arrives (SLEW, TRACK, or MOVE_AXIS), enters motion_loop() which dispatches
 * to the appropriate RMT-driven execution path.
 * Stop / park are handled directly by their callers via
 * motors_motion_stop() + motors_state update.
 * -------------------------------------------------------------------------- */
static void motors_motion_task_run(void *arg) {
    (void) arg;

    /* Initialize RMT from this task — pins the RMT ISR to core 1
     * (isolated from everything else), eliminating ISR latency jitter. */
    esp_err_t rmt_err = motors_rmt_init();
    if (rmt_err != ESP_OK) {
        ESP_LOGE(TAG, "motors_rmt_init failed: %s — entering ERROR state",
                 esp_err_to_name(rmt_err));
        motors_enter_hardware_fault();
        motors_motion_task_handle = NULL;  /* clear the dangling handle */
        vTaskDelete(NULL);
        return;
    }
    while (true) {
        /* Publish the stop outcome BEFORE signalling idle.  The drain result and
         * its identity (s_motion.drain_generation) are captured at the end of
         * motion_loop(), at the moment the motion actually ended — so a STOP that
         * times out and releases the control mutex, letting a later STOP advance
         * the generation while this task is held (or preempted) before the
         * barrier, can never re-tag the drain result with a foreign identity.
         * The rest generation is read here, so a STOP that landed while the task
         * was held is still confirmed at the current generation. */
        motors_test_hook_before_idle_publish();
        uint32_t rest_gen = motors_stop_generation_get();
        portENTER_CRITICAL(&motors_state_lock);
        /* Current rest generation — re-published every barrier so a STOP that
         * lands while the task is already idle is confirmed on the next poll. */
        s_idle_generation = rest_gen;
        if (s_motion.ran) {
            /* The finalisation motive is the snapshot captured at the end of
             * motion_loop, not the live flags — a STOP that lands after the
             * motion ended must not rewrite the reason it ended with. */
            bool graceful = s_motion.drain_graceful;
            bool motion_aborted = s_motion.drain_aborted;
            /* The drain result belongs to the generation captured at the moment
             * the motion ended (s_motion.drain_generation), not the generation
             * current at publish time.  Only a drain request (graceful) yields an
             * abort result: a plain setpoint-version preempt (graceful=false,
             * aborted=true) is not a failed drain and must not leak into a later
             * wait.  The record is NOT cleared on an idle re-poll, so a published
             * failure stays associated with its own generation instead of being
             * erased into a false success. */
            s_idle_aborted_generation = s_motion.drain_generation;
            s_idle_aborted = graceful && motion_aborted;
            s_motion.aborted = false;
            s_motion.ran = false;
            /* A MoveAxis whose session was finalised (graceful) publishes READY
             * here, before any queued command is dequeued — so a later SLEW/TRACK
             * started independently is never overwritten by a late producer write
             * — and publishes the explicit session completion.  A setpoint
             * preemption (not graceful) drains but does NOT finalise the session,
             * so this block is skipped for it. */
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
                    /* The drain failed (position uncertain), whether the motion
                     * was a graceful stop or a setpoint preemption.  Close
                     * admission at the point of detection, so no work can be
                     * admitted or processed between the executor publishing the
                     * failure and the requester consuming it.  Idempotent: a STOP
                     * drain already has admission closed. */
                    motors_admission_close();
                }
            }
        }
        /* Publish READY for a motion that just completed, here at the barrier
         * where the task is formally at rest.  A resume-tracking transition
         * (TRACKING with a live mode), a latched fault or a park must not be
         * overwritten. */
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
        /* Block for at most MOTION_IDLE_QUEUE_POLL_MS so a STOP that lands while
         * already idle re-publishes the idle generation above (confirming rest
         * at the new generation) instead of sleeping past it. */
        if (xQueueReceive(motion_cmd_queue, &cmd,
                          pdMS_TO_TICKS(MOTION_IDLE_QUEUE_POLL_MS)) != pdTRUE) {
            continue;
        }

        /* The motion task is now running.  Published under the same lock that
         * guards the idle read in motors_motion_wait_idle, so the transition
         * idle→running is coherent with a concurrent rest wait. */
        portENTER_CRITICAL(&motors_state_lock);
        s_motion_idle = false;
        portEXIT_CRITICAL(&motors_state_lock);

        process_command(cmd);

        /* Test hook: pause here to exercise a STOP after the command was
         * dequeued and validated but before the motion loop activates it. */
        motors_test_hook_after_process_command();

        if (cmd.type == MOTION_CMD_SLEW || cmd.type == MOTION_CMD_TRACK ||
            cmd.type == MOTION_CMD_MOVE_AXIS) {
            motion_loop();
        }
        /* PULSE_GUIDE: the tracking loop picks up guide state on each
         * iteration.  If tracking is NOT active, the guide pulse runs
         * via a minimal single-step loop below. */
        if (cmd.type == MOTION_CMD_PULSE_GUIDE
            && motors_state.status != MOTORS_STATUS_TRACKING) {
            /* Standalone guiding — run a short motion loop just for
             * the guide pulse.  DEC-only: no tracking base rate. */
            motion_loop();
        }

        /* Clear guiding flag when both axes are idle. */
        bool ra_active = (s_motion.ra_guide_deadline_us != 0)
                      && (esp_timer_get_time() < s_motion.ra_guide_deadline_us);
        bool dec_active = (s_motion.dec_guide_deadline_us != 0)
                       && (esp_timer_get_time() < s_motion.dec_guide_deadline_us);
        motors_state.guiding = (ra_active || dec_active);
    }
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

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

/*
 * Wait (bounded) until the motion task confirms rest at `generation` or later,
 * so a caller can safely modify counters/limits without racing the in-flight
 * motion loop.  Callers must invoke motors_stop_and_wait() (which advances the
 * generation and aborts the motion) before this; it then blocks until the task
 * has left its loop and re-published the idle generation.  The confirmation and
 * the abort result for that generation are returned together in one coherent
 * read.
 */
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
            /* Rest is confirmed at `generation` (or later).  The drain result is
             * attributed only to the generation it belongs to: a later generation's
             * confirmation never hands a stale result to an earlier request, and a
             * clean stop on an already-idle mount (no motion ran) reports no abort. */
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

/*
 * Wait (bounded) until the executor publishes the explicit completion of the
 * MoveAxis session `session` (or a later one): either the session's motion
 * drained gracefully, or its command was discarded.  A setpoint-version preempt
 * or an unrelated idle barrier never publishes a completion, so this cannot
 * return prematurely.  Returns the drain outcome.
 */
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

/*
 * Observability — read the last published manual-session completion under the
 * same lock that guards its publication, so a test (or a future diagnostic) can
 * assert the completion is stable and tied to the right session.  The session
 * identity is monotonic: it only ever advances, never regresses or erases.
 */
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

/*
 * Observability — read the last published global stop confirmation (idle
 * generation + drain result) under the same lock that guards its publication.
 */
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

/* The generation the last published drain result belongs to.  Together with
 * motors_idle_aborted() this is the stable confirmation record: a test can assert
 * the result stays tied to its own generation across idle re-polls. */
uint32_t motors_idle_aborted_generation(void) {
    uint32_t gen;
    portENTER_CRITICAL(&motors_state_lock);
    gen = s_idle_aborted_generation;
    portEXIT_CRITICAL(&motors_state_lock);
    return gen;
}

/* Test-only: clear the stable drain-result record.  Production never clears it
 * mid-session (a confirmation, once published, stays associated with its own
 * generation); the host harness reuses one process across many tests and must
 * start each from a clean record. */
void motors_reset_idle_completion(void) {
    portENTER_CRITICAL(&motors_state_lock);
    s_idle_aborted = false;
    s_idle_aborted_generation = 0;
    portEXIT_CRITICAL(&motors_state_lock);
}
