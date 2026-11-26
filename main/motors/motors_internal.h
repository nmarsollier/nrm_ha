#pragma once

#include "motors.h"

#include "motion/axis_units.h"

#include "driver/rmt_tx.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/* =========================================================================
 * Motion command queue — thread-safe communication with the motion task.
 *
 * External callers (protocol handlers, button poller) send MotionCommand
 * structs to the queue.  The motors task is the sole consumer and the
 * sole writer of motors_state position fields.
 *
 * Only motion-producing commands go through the queue.  Stop / park
 * are handled directly by their callers via
 * motors_motion_stop() + motors_state update — no queue round-trip.
 * ========================================================================= */

typedef enum {
    MOTION_CMD_SLEW = 0,
    MOTION_CMD_TRACK,
    MOTION_CMD_MOVE_AXIS,
    MOTION_CMD_PULSE_GUIDE,
} MotionCommandType;

typedef struct {
    MotionCommandType type;
    float ra_target_deg;
    float dec_target_deg;
    float ra_speed;
    float dec_speed;
    TrackingMode tracking_mode;
    /* SLEW: tracking mode to restore once the slew completes successfully.
     * Carried by the command so the resume is atomic with the slew, not a
     * separate order that a queue-full or a concurrent STOP could drop. */
    TrackingMode resume_tracking;
    bool relative;
    float ra_delta_deg;
    float dec_delta_deg;
    /* PulseGuide fields (valid when type == MOTION_CMD_PULSE_GUIDE) */
    int guide_axis;        /* 0 = RA, 1 = DEC */
    float guide_offset_dps; /* signed deg/s */
    uint32_t guide_duration_ms;
    /* Stop epoch captured at enqueue — a command whose epoch is stale
     * (a STOP landed after it was enqueued) is discarded at execution. */
    uint32_t generation;
    /* Manual-session identity captured at enqueue — a MoveAxis order whose
     * session no longer matches the active one is discarded at execution (a
     * global STOP generation no longer distinguishes two manual sessions). */
    uint32_t session;
} MotionCommand;

/* Queue handle — created by motors_init(), shared across the module. */
extern QueueHandle_t motion_cmd_queue;

/* Stop epoch — incremented on every stop so queued commands can be
 * invalidated.  Read/written from multiple tasks (uint32, monotonic). */
extern volatile uint32_t motors_stop_generation;

/* MoveAxis continuous-move setpoint (signed deg/s) + version.  The newest
 * value is authoritative: the producer overwrites it and bumps the version, and
 * the motion task reads it directly, so obsolete MoveAxis orders never
 * accumulate behind interleaved commands. */
extern volatile float motors_move_axis_ra_dps;
extern volatile float motors_move_axis_dec_dps;
extern volatile uint32_t motors_move_axis_version;

/* Manual-session state — owned by motors_admission_lock together with the
 * setpoint and the stop generation: the session opens atomically with MoveAxis
 * acceptance (motors_queue_put_move_axis) and closes atomically with STOP/PARK
 * (motors_stop_begin) or the zero that ends the session (motors_move_axis_stop).
 * Every reader and writer must hold motors_admission_lock. */
extern volatile bool motors_manual_move;
extern TrackingMode motors_saved_tracking;
extern volatile uint32_t motors_manual_session;

/* Spinlock guarding the int64_t position counters against torn cross-core
 * reads (motors_current_state / motors_get_*_deg). */
extern portMUX_TYPE motors_state_lock;

/* Spinlock guarding command admission and the stop generation.  A producer that
 * enqueues a command and a STOP that closes admission are linearised against
 * each other through this lock: either the command is enqueued with the pre-stop
 * generation (and discarded by the task), or admission is already closed and the
 * command is rejected. */
extern portMUX_TYPE motors_admission_lock;

/* Send a MotionCommand to the back of the queue (FIFO).  The admission check,
 * the generation stamp and the (non-blocking) enqueue happen atomically under
 * motors_admission_lock, so no command can slip between a STOP's admission close
 * and its generation bump.  Returns false if the queue is full, uninitialised,
 * or admission is closed. */
bool motors_queue_put(MotionCommand *cmd);

/* Enqueue a MOVE_AXIS command and publish its setpoint (rate + version) and the
 * manual session as one admission transaction, so a STOP cannot clear the
 * command between the enqueue and the setpoint/session publication — which would
 * orphan the setpoint and spuriously preempt the running move, or leave a
 * phantom session after the stop.  Returns false on a full queue or closed
 * admission, leaving the setpoint and session untouched. */
bool motors_queue_put_move_axis(MotionCommand *cmd, float ra_speed, float dec_speed);

/* Enqueue a MOVE_AXIS command and publish its setpoint + session assuming
 * motors_admission_lock is already held.  Exposed so the single-axis update can
 * read, modify and publish a setpoint as one atomic admission transaction. */
bool motors_queue_put_move_axis_under_lock(MotionCommand *cmd, float ra_speed, float dec_speed);

/* Enqueue a command assuming motors_admission_lock is already held.  Performs
 * the admission check (s_stopping), the generation stamp and the non-blocking
 * enqueue.  Exposed so a caller can validate a condition and insert a command in
 * one critical section. */
bool motors_queue_put_under_lock(MotionCommand *cmd);

/* Atomically verify no new manual session was admitted and enqueue the tracking
 * restore for `mode`.  A new manual session is a successful skip (MOTOR_OK);
 * otherwise the enqueue result is returned (MOTOR_OK, or MOTOR_ERR_BUSY on a
 * full queue / closed admission).  The check and the enqueue are one
 * admission-lock critical section, so a MoveAxis admitted concurrently cannot
 * leave a stale TRACK queued behind the new session. */
MotorResultCode motors_restore_tracking_if_idle(TrackingMode mode);

/* Begin a stop window: atomically close command admission, advance the stop
 * generation, and invalidate the manual session (manual_move + saved_tracking),
 * so a MoveAxis accepted before this stop is definitively cancelled.  Returns
 * the new generation, which identifies this stop and is used to confirm rest
 * and reject stale commands. */
uint32_t motors_stop_begin(void);

/* Close command admission and invalidate queued + manual work (generation bump),
 * for a hardware fault or a failed drain that makes new motion unsafe.  Idempotent:
 * does not advance the generation if admission is already closed.  Takes
 * motors_admission_lock only. */
void motors_admission_close(void);

/* True when command admission is currently closed (the s_stopping gate). */
bool motors_admission_is_closed(void);

/* Reopen command admission only when no ERROR is latched.  Returns true when
 * admission was reopened, false when ERROR keeps it closed.  Must only be called
 * after rest has been confirmed; a timeout without confirmed rest must leave
 * admission closed. */
bool motors_admission_open(void);

/* Reopen the admission gate, assuming motors_state_lock is already held.  Shared
 * by motors_admission_open() and motors_recover_to_ready(). */
void motors_admission_open_locked(void);

/* Atomically discard every command in the queue. */
void motors_queue_clear(void);

/* Validate axis values against the configured inclusive limits. */
bool motors_is_valid_ra(float value);

bool motors_is_valid_dec(float value);

bool motors_is_valid_ra_steps(int64_t steps);

bool motors_is_valid_dec_steps(int64_t steps);

/* =========================================================================
 * Mechanical constants — hardware configuration.
 * ========================================================================= */

/* Maximum safe slew speed in deg/s — hardware ceiling for this reduction. */
#define MOTORS_MAX_SLEW_SPEED_DPS 6.0f

/* Maximum manual move-axis speed in deg/s — ceiling for continuous
 * MoveAxis / joystick motion.  Lower than the slew ceiling because manual
 * motion runs at constant velocity (no acceleration ramp). */
#define MOTORS_MAX_MOVE_AXIS_SPEED_DPS 1.0f

/*
 * Angular displacement per microstep for one mount axis.
 *
 *   full_step_deg        = NEMA 17 full-step angle (1.8°)
 *   canonical_microsteps = 64 (TMC2209 CHOPCONF.MRES)
 *   gear_ratio           = total reduction (motor shaft : axis)
 *
 * Each axis reads its own AxisProfile so RA and DEC can have independent
 * reductions.
 */
static inline float motors_get_deg_per_microstep(AxisId axis) {
    return (float) axis_deg_per_step(axis);
}

/* =========================================================================
 * Runtime microstep resolution (motors_microsteps.c)
 *
 * The STEP counters stay in the profile's canonical (tracking) microsteps
 * regardless of the active driver resolution.  A large ramped slew switches
 * both drivers to the profile's slew microsteps and emits one STEP pulse per
 * `motors_steps_per_pulse()` canonical steps; every other motion runs at the
 * canonical resolution (factor 1).
 * ========================================================================= */

/* Canonical counter steps advanced by a single STEP pulse at the active resolution. */
uint32_t motors_steps_per_pulse(void);

/* Select the resolution and chopper mode for a motion command: a ramped SLEW
 * uses the coarse resolution + SpreadCycle when either axis travels more than
 * the profile's gentle-slew threshold; every other command type uses fine +
 * StealthChop.  `ra_dist`/`dec_dist` are the canonical axis distances
 * (meaningful only for SLEW).  Returns false (and puts the mount into ERROR)
 * when a driver switch could not be verified. */
bool motors_microsteps_prepare(MotionCommandType type, int64_t ra_dist, int64_t dec_dist);

/* =========================================================================
 * Slew ramp profile — the parameters live in the mount profile
 * (mount_profile.slew), so the motion task's ramp_velocity() and the
 * arrival-time estimator (slew_ramp_duration) share the same definition.
 * ========================================================================= */

/* =========================================================================
 * Hardware layer — DIR GPIO control (motors_hw.c).
 *
 * STEP pulse generation is handled by the RMT peripheral (motors_rmt.c).
 * The TMC2209 drivers are always enabled (no EN pin connected), so there is
 * no enable GPIO to manage.  ALARM pins are not connected.
 * ========================================================================= */

typedef enum {
    MOTOR_DIRECTION_NEGATIVE = 0,
    MOTOR_DIRECTION_POSITIVE = 1,
} MotorDirection;

esp_err_t motors_hw_init(void);

void motors_hw_set_direction_ra(MotorDirection direction);

void motors_hw_set_direction_dec(MotorDirection direction);

/* Test-only: clear the DIR write cache (motors_hw.c). */
void motors_hw_reset_dir_cache(void);

/* =========================================================================
 * RMT+DMA step pulse generation (motors_rmt.c).
 *
 * Replaces software GPIO bit-banging with hardware-timed RMT symbols
 * streamed via GDMA. Zero jitter, near-zero CPU.
 * ========================================================================= */

/* RMT clock resolution — 2 MHz (0.5 us per tick).
 * Balanced for high-reduction configurations while keeping slow-step
 * idle symbols within SOC_RMT_MEM_WORDS_PER_CHANNEL (48). */
#define RMT_RESOLUTION_HZ 2000000U

/* STEP pulse timing in RMT ticks (2 MHz reference).
 *
 * Driver (TMC2209): one microstep per accepted STEP pulse (rising edge).
 * The STEP input is 3.3 V CMOS, driven directly (no level shifting), so a
 * 2 µs HIGH pulse is ample margin. */
#define STEP_PULSE_TICKS     4U    /* 2 us HIGH — TMC2209 direct drive */
#define STEP_MIN_LOW_TICKS   4U    /* 2 us LOW floor */
#define STEP_MIN_PERIOD_TICKS (STEP_PULSE_TICKS + STEP_MIN_LOW_TICKS)  /* 8 ticks = 4 us */

/* =========================================================================
 * Position representation — int64_t absolute microstep counters.
 *
 * Degrees are a derived view over the step counter, computed on demand
 * for API consumers.  All internal position tracking uses integer steps
 * for zero-accumulation-error precision over arbitrarily long sessions.
 * ========================================================================= */

/* Convert between steps and degrees using the active microstep resolution.
 * Defined in motors_current_state.c; declared public in motors.h. */
float motors_steps_to_deg(AxisId axis, int64_t steps);

static inline int64_t motors_deg_to_steps(AxisId axis, float degrees) {
    int64_t steps;
    /* Callers keep the angle finite and within the mechanical limits, so this
     * conversion never fails in practice; on a defensive failure, fall back to
     * 0 (home) rather than propagate an invalid step count into a distance. */
    if (!axis_deg_to_steps(axis, (double) degrees, &steps)) {
        return 0;
    }
    return steps;
}

esp_err_t motors_rmt_init(void);

uint32_t motors_rmt_encode_steps(rmt_symbol_word_t *symbols,
                                  uint32_t max_symbols,
                                  uint32_t step_period_ticks,
                                  uint32_t step_count);

/*
 * Encode a bare STEP pulse (HIGH + minimal LOW).
 * For use by the tracking/guiding loop where inter-step timing
 * is handled by the accumulator + deadline, not by RMT symbols.
 * Always consumes 1 symbol.
 */
uint32_t motors_rmt_encode_pulse(rmt_symbol_word_t *symbols);

esp_err_t motors_rmt_transmit_ra(const rmt_symbol_word_t *symbols,
                                  uint32_t num_symbols);

esp_err_t motors_rmt_transmit_dec(const rmt_symbol_word_t *symbols,
                                   uint32_t num_symbols);

/*
 * Pipelined transmit — no semaphore drain before launching.
 * Used by the double-buffered slewing loop after the first batch.
 */
esp_err_t motors_rmt_transmit_no_drain_ra(const rmt_symbol_word_t *symbols,
                                           uint32_t num_symbols);

esp_err_t motors_rmt_transmit_no_drain_dec(const rmt_symbol_word_t *symbols,
                                            uint32_t num_symbols);

esp_err_t motors_rmt_wait_ra(TickType_t timeout_ticks);

esp_err_t motors_rmt_wait_dec(TickType_t timeout_ticks);

bool motors_rmt_try_wait_ra(void);

bool motors_rmt_try_wait_dec(void);

/* Recreate both RMT channels — motion task only. */
void motors_rmt_reset_both(void);

/* =========================================================================
 * Module-global state — motors_state is the single source of truth for
 * the motors layer.  External code reads it through motors_current_state().
 * ========================================================================= */
extern MotorsState motors_state;

/* Latched hardware-fault tracking — see motors_enter_error_state.c.
 * A hardware fault is never cleared by a power-loss recovery. */
void motors_enter_hardware_fault(void);
bool motors_has_hardware_fault(void);
/* Recover from a non-hardware ERROR (power loss) to READY and reopen admission,
 * only if no hardware fault is latched.  Atomic under motors_state_lock; the
 * caller holds the control mutex. */
void motors_recover_to_ready(void);
/* Test-only: clear the latched fault (production clears it only on reboot). */
void motors_reset_hardware_fault(void);

/* Mark the current ERROR as a recoverable power fault so a later power-on
 * re-initialises the drivers (motors_power.c). */
void motors_mark_power_fault(void);

float motors_get_tracking_speed(TrackingMode mode);

/* Motion task handle — exposed so external code can send notifications. */
extern TaskHandle_t motors_motion_task_handle;

/* =========================================================================
 * Task & queue lifecycle (motors_task.c, motors_queue.c).
 * ========================================================================= */

/* Stop the active motion loop from outside the motion task.
 * Owns s_motion.active and task notification.  Does not touch admission or the
 * stop generation — call motors_stop_begin() first for a full stop window.
 * Safe to call from any task. */
void motors_motion_stop(void);

/* Mark the running motion as gracefully stopped (active=false, graceful=true),
 * called with motors_admission_lock already held so the STOP path can bump the
 * generation and set this flag in one coherent critical section. */
void motors_motion_mark_stopped_locked(void);

/* Full stop request shared by STOP/PARK/reset: close admission, advance the
 * generation, clear the queue, abort the running motion and wait for the motion
 * task to confirm rest at that generation.  Returns MOTOR_OK with admission
 * still closed (the caller publishes the final state and reopens it), or an
 * error.  On a timeout rest is NOT confirmed and admission is left closed. */
MotorResultCode motors_stop_and_wait(void);

/* Manual-session finalization, shared by the full-zero stop (motors_move_axis_stop)
 * and the single-axis zero (motors_set_move_axis_rate).  Split in two halves:
 * the atomic capture + invalidation under motors_admission_lock, and the drain
 * wait + result interpretation + tracking restore outside the spinlock. */
void motors_manual_invalidate_locked(uint32_t *session, TrackingMode *restore);
MotorResultCode motors_manual_finish(uint32_t session, TrackingMode restore);

/* Read the stop generation under the admission lock (the same mechanism as the
 * writes), so a reader on another core sees the latest bump coherently. */
uint32_t motors_stop_generation_get(void);

/* Wait (bounded) until the motion task confirms rest at `generation` or later,
 * returning the confirmation and the abort result together (one coherent read).
 * On success returns true and fills `*aborted`; on timeout returns false and
 * leaves `*aborted` untouched.  The caller must NOT reopen admission on timeout. */
bool motors_motion_wait_idle(uint32_t generation, bool *aborted);

/* Wait (bounded) until the executor publishes the explicit completion of the
 * MoveAxis session `session` (or a later one): its motion drained gracefully, or
 * its command was discarded.  Independent of the stop generation, so a normal
 * manual finalization confirms its completion without advancing the global
 * generation.  Returns the drain outcome (true if the drain aborted). */
bool motors_motion_wait_drained(uint32_t session, bool *aborted);

/* Observability — read the last published manual-session completion (guarded by
 * motors_state_lock): the completed session identity and whether its drain
 * aborted.  Exposed so tests can assert the completion is stable, monotonic and
 * tied to the session that was finalised. */
uint32_t motors_finished_manual_session(void);
bool motors_finished_manual_aborted(void);

/* Observability — read the last published global stop confirmation. */
uint32_t motors_idle_generation(void);
bool motors_idle_aborted(void);
/* The generation the last published drain result belongs to (stable record). */
uint32_t motors_idle_aborted_generation(void);
/* Test-only: clear the stable drain-result record (production never clears it). */
void motors_reset_idle_completion(void);

/* Serialise complete control operations (STOP/PARK/limits) against each other,
 * so one operation cannot reopen admission while another is still finalising.
 * A FreeRTOS mutex (not a spinlock): held across the bounded rest-wait, never
 * across UART/NVS/RMT.  Create it once with motors_control_init() during
 * startup — never lazily. */
esp_err_t motors_control_init(void);
bool motors_control_lock(void);
void motors_control_unlock(void);

esp_err_t motors_motion_task_init(void);

esp_err_t motors_queue_init(void);
