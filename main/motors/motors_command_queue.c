/* Motors - motors_command_queue.c
 *
 * Purpose: FreeRTOS command queue for the motion task.
 * Owns the queue handle, lifecycle, send, and clear helpers.
 */

#include "motors.h"
#include "motors_internal.h"

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

static const char *TAG = "MOTORS_MOTION_CMD";

/* Queue handle — shared across the motors module via motors_internal.h. */
QueueHandle_t motion_cmd_queue = NULL;

/* Admission gate — while a STOP/PARK is in progress no new motion command is
 * accepted.  Guarded by motors_admission_lock together with the stop generation,
 * so "admission closed" and "generation bumped" are a single atomic step. */
static volatile bool s_stopping = false;

/* Serialises command admission against stop generation, see motors_internal.h. */
portMUX_TYPE motors_admission_lock = portMUX_INITIALIZER_UNLOCKED;

/* Close admission, advance the stop generation and invalidate the manual session
 * + saved tracking.  Assumes motors_admission_lock is already held. */
static void admission_close_unlocked(void) {
    s_stopping = true;
    motors_stop_generation++;
    motors_manual_move = false;
    motors_saved_tracking = TRACKING_NONE;
}

uint32_t motors_stop_begin(void) {
    portENTER_CRITICAL(&motors_admission_lock);
    admission_close_unlocked();
    /* Mark the running motion graceful in the SAME critical section as the
     * generation bump, so the finalisation capture (which reads generation and
     * graceful under this lock) can never observe a new generation with a stale
     * graceful flag. */
    motors_motion_mark_stopped_locked();
    uint32_t gen = motors_stop_generation;
    portEXIT_CRITICAL(&motors_admission_lock);
    return gen;
}

uint32_t motors_stop_generation_get(void) {
    portENTER_CRITICAL(&motors_admission_lock);
    uint32_t gen = motors_stop_generation;
    portEXIT_CRITICAL(&motors_admission_lock);
    return gen;
}

/*
 * Close command admission and invalidate queued + manual work through the locked
 * generation mechanism.  Used when a hardware fault or a failed drain makes new
 * motion unsafe.  Idempotent: if admission is already closed (e.g. by a STOP that
 * is draining), it does not advance the generation again for the same event.
 * Safe to call with or without motors_state_lock already held (it only takes
 * motors_admission_lock).
 */
void motors_admission_close(void) {
    portENTER_CRITICAL(&motors_admission_lock);
    if (!s_stopping) {
        admission_close_unlocked();
    }
    portEXIT_CRITICAL(&motors_admission_lock);
}

/* True when command admission is currently closed (the s_stopping gate). */
bool motors_admission_is_closed(void) {
    bool closed;
    portENTER_CRITICAL(&motors_admission_lock);
    closed = s_stopping;
    portEXIT_CRITICAL(&motors_admission_lock);
    return closed;
}

/*
 * Reopen the admission gate, assuming motors_state_lock is already held.
 * Shared by motors_admission_open() and motors_recover_to_ready(), which must
 * publish READY and reopen admission atomically.
 */
void motors_admission_open_locked(void) {
    portENTER_CRITICAL(&motors_admission_lock);
    s_stopping = false;
    portEXIT_CRITICAL(&motors_admission_lock);
}

/*
 * Reopen command admission only when no ERROR is latched.  Returns true when
 * admission was reopened, false when ERROR keeps it closed.  motors_state_lock
 * is held while motors_admission_lock is acquired, matching the state→admission
 * order of motors_enter_error_state(), so a concurrent ERROR cannot slip between
 * the error check and the reopen.
 */
bool motors_admission_open(void) {
    portENTER_CRITICAL(&motors_state_lock);
    if (motors_status_is_error(motors_state.status)) {
        portEXIT_CRITICAL(&motors_state_lock);
        return false;
    }
    motors_admission_open_locked();
    portEXIT_CRITICAL(&motors_state_lock);
    return true;
}

/* --------------------------------------------------------------------------
 * Queue lifecycle.
 * -------------------------------------------------------------------------- */
esp_err_t motors_queue_init(void) {
    motion_cmd_queue = xQueueCreate(10, sizeof(MotionCommand));
    if (motion_cmd_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create motion command queue");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * Shared helper — send a command to the motion task queue.
 *
 * Every command is sent to the back (FIFO order).  STOP / PARK / DISABLE
 * callers reset the queue before sending so they always run immediately.
 * -------------------------------------------------------------------------- */
bool motors_queue_put_under_lock(MotionCommand *cmd) {
    if (s_stopping) {
        return false;
    }
    cmd->generation = motors_stop_generation;
    return (xQueueSendToBack(motion_cmd_queue, cmd, 0) == pdTRUE);
}

bool motors_queue_put(MotionCommand *cmd) {
    if (motion_cmd_queue == NULL) {
        ESP_LOGE(TAG, "Motion command queue not initialized");
        return false;
    }

    /* Admission check + generation stamp + enqueue are one critical section, so
     * a concurrent STOP (which closes admission and bumps the generation under
     * the same lock) can never land between the check and the enqueue. */
    portENTER_CRITICAL(&motors_admission_lock);
    bool ok = motors_queue_put_under_lock(cmd);
    portEXIT_CRITICAL(&motors_admission_lock);

    if (!ok) {
        ESP_LOGW(TAG, "Queue full or admission closed, dropping cmd type=%d", cmd->type);
    }
    return ok;
}

bool motors_queue_put_move_axis(MotionCommand *cmd, float ra_speed, float dec_speed) {
    if (motion_cmd_queue == NULL) {
        ESP_LOGE(TAG, "Motion command queue not initialized");
        return false;
    }

    /* Enqueue + setpoint + manual-session publication are one critical section,
     * so a STOP that lands between them cannot clear the command and leave the
     * setpoint orphaned (which would spuriously preempt the running move) nor
     * leave a phantom session after the stop. */
    portENTER_CRITICAL(&motors_admission_lock);
    bool ok = motors_queue_put_move_axis_under_lock(cmd, ra_speed, dec_speed);
    portEXIT_CRITICAL(&motors_admission_lock);

    if (!ok) {
        ESP_LOGW(TAG, "Queue full or admission closed, dropping MOVE_AXIS");
    }
    return ok;
}

/*
 * Enqueue a MOVE_AXIS command and publish its setpoint + manual-session identity
 * assuming motors_admission_lock is already held.  The read-modify-write of the
 * single-axis update publishes through this helper so its read, modification and
 * publication stay a single atomic transaction.  Returns false on a closed
 * admission or a full queue, leaving the setpoint and session untouched.
 */
bool motors_queue_put_move_axis_under_lock(MotionCommand *cmd, float ra_speed, float dec_speed) {
    if (s_stopping) {
        return false;
    }
    cmd->generation = motors_stop_generation;
    /* Open a new manual session (bump its identity) only if none is active; an
     * update keeps the current identity.  The identity is committed only when the
     * enqueue succeeds, so a rejected order never wastes a session. */
    uint32_t session = motors_manual_session;
    if (!motors_manual_move) {
        session++;
    }
    cmd->session = session;
    bool ok = (xQueueSendToBack(motion_cmd_queue, cmd, 0) == pdTRUE);
    if (ok) {
        motors_manual_session = session;
        motors_manual_move = true;
        motors_move_axis_ra_dps = ra_speed;
        motors_move_axis_dec_dps = dec_speed;
        motors_move_axis_version++;
    }
    return ok;
}

/* Atomically verify no new manual session was admitted and enqueue the tracking
 * restore.  A new manual session is the successful "skip" outcome (MOTOR_OK);
 * otherwise the restore is enqueued and the result of that enqueue is returned.
 * The check and the enqueue are one admission-lock critical section, so a
 * MoveAxis admitted concurrently cannot leave a stale TRACK queued behind the
 * new session. */
MotorResultCode motors_restore_tracking_if_idle(TrackingMode mode) {
    if (motion_cmd_queue == NULL) {
        return MOTOR_ERR_HARDWARE_ERROR;
    }

    MotionCommand cmd = {
        .type = MOTION_CMD_TRACK,
        .ra_target_deg = 0.0f,
        .dec_target_deg = 0.0f,
        .ra_speed = motors_get_tracking_speed(mode),
        .dec_speed = 0.0f,
        .tracking_mode = mode,
    };

    portENTER_CRITICAL(&motors_admission_lock);
    if (motors_manual_move) {
        /* A new manual session was admitted: skipping the restore is the
         * successful outcome, not an error. */
        portEXIT_CRITICAL(&motors_admission_lock);
        return MOTOR_OK;
    }
    bool ok = motors_queue_put_under_lock(&cmd);
    portEXIT_CRITICAL(&motors_admission_lock);

    return ok ? MOTOR_OK : MOTOR_ERR_BUSY;
}

/* --------------------------------------------------------------------------
 * Public — atomically empty the command queue.
 * -------------------------------------------------------------------------- */
void motors_queue_clear(void) {
    if (motion_cmd_queue != NULL) {
        xQueueReset(motion_cmd_queue);
    }
}
