/* Motors - motors_stop.c
 *
 * Purpose: stop all motor movement immediately.
 *
 * The stop is a barrier: admission is closed and the stop generation advanced
 * atomically, the queue cleared and the running motion aborted, then READY is
 * published only after the motion task confirms rest at that generation.
 */
#include "motors.h"
#include "motors_internal.h"

#include "freertos/semphr.h"

/* Test hook — weak no-op overridden by the host harness to pause a stop after
 * admission is closed and the motion aborted, before the idle wait. */
void __attribute__((weak)) motors_test_hook_after_stop_begin(void) {}

/* Test hooks defined (weak) in motors_move_axis_speed.c, now shared by the
 * manual-finalization helpers below. */
void motors_test_hook_after_move_axis_stop_open(void);
void motors_test_hook_before_move_axis_tracking_restore(void);

/* Serialises complete control operations (STOP/PARK/limits) against each other,
 * so one operation cannot reopen admission while another is still finalising.
 * A FreeRTOS mutex (not a spinlock): held across the bounded rest-wait, never
 * across UART/NVS/RMT. */
static SemaphoreHandle_t s_control_mutex = NULL;

esp_err_t motors_control_init(void) {
    if (s_control_mutex == NULL) {
        s_control_mutex = xSemaphoreCreateMutex();
        if (s_control_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

bool motors_control_lock(void) {
    return (s_control_mutex != NULL)
           && (xSemaphoreTake(s_control_mutex, portMAX_DELAY) == pdTRUE);
}

void motors_control_unlock(void) {
    if (s_control_mutex != NULL) {
        xSemaphoreGive(s_control_mutex);
    }
}

/*
 * Full stop request shared by STOP/PARK and the limits/reset paths.
 *
 * Closes admission and advances the stop generation atomically, clears the
 * queue, aborts the running motion and waits for the motion task to confirm
 * rest at that generation.  On MOTOR_OK the caller still holds admission closed
 * and must publish the final state and reopen admission.  On a timeout rest is
 * NOT confirmed and admission is left closed — never reopened without rest.
 */
MotorResultCode motors_stop_and_wait(void) {
    /* ERROR is unrecoverable — never transition out of it. */
    if (motors_status_is_error(motors_state.status)) {
        return MOTOR_ERR_HARDWARE_ERROR;
    }

    /* motors_stop_begin() closes admission, advances the stop generation and
     * invalidates the manual session (manual_move + saved_tracking) atomically. */
    uint32_t gen = motors_stop_begin();
    motors_queue_clear();
    motors_motion_stop();

    /* Test hook: pause here (admission already closed, generation already
     * advanced, motion already aborted) to exercise a concurrent enqueue. */
    motors_test_hook_after_stop_begin();

    bool aborted = false;
    if (!motors_motion_wait_idle(gen, &aborted)) {
        /* Rest not confirmed — keep admission closed so no command can race a
         * motion task that may still be emitting steps. */
        return MOTOR_ERR_NOT_READY;
    }

    /* A hardware fault during the drain, or a failed drain (lost position),
     * must never be reported as a clean stop.  Admission stays closed — a fault
     * or a failed drain does not authorise new motion. */
    if (motors_status_is_error(motors_state.status)) {
        return MOTOR_ERR_HARDWARE_ERROR;
    }
    if (aborted) {
        return MOTOR_ERR_NOT_READY;
    }

    /* Rest confirmed and clean — admission stays closed until the caller
     * publishes the final state. */
    return MOTOR_OK;
}

/*
 * Manual-session finalization — stop only the MoveAxis session and reconcile its
 * committed pulses, without the global-stop side effects.  Admission is NOT
 * closed, the stop generation is NOT advanced, and the queue is NOT cleared, so
 * unrelated accepted commands survive and a new session may be admitted while
 * this finalization drains.
 *
 * The finalization is split in two halves so the full-zero stop and the
 * single-axis zero share exactly one implementation:
 *   motors_manual_invalidate_locked() — atomic capture + invalidation, under
 *       motors_admission_lock;
 *   motors_manual_finish() — drain wait, result interpretation, hooks and
 *       tracking restore, outside the spinlock.
 */

/* Called with motors_admission_lock held.  Captures the session identity and the
 * tracking to restore, then invalidates the session and zeroes both setpoints. */
void motors_manual_invalidate_locked(uint32_t *session, TrackingMode *restore) {
    *session = motors_manual_session;
    *restore = motors_saved_tracking;
    motors_manual_move = false;
    motors_saved_tracking = TRACKING_NONE;
    motors_move_axis_ra_dps = 0.0f;
    motors_move_axis_dec_dps = 0.0f;
}

/*
 * Wait for the executor to publish the explicit completion of the invalidated
 * session `session`, interpret the outcome (timeout / hardware fault / aborted
 * drain), run the finalization hooks and restore the saved tracking.  Must be
 * called OUTSIDE motors_admission_lock, with the control mutex held.
 */
MotorResultCode motors_manual_finish(uint32_t session, TrackingMode restore) {
    bool aborted = false;
    if (!motors_motion_wait_drained(session, &aborted)) {
        /* Timeout: the executor never confirmed completion — unsafe to admit
         * new motion, so keep admission closed. */
        motors_admission_close();
        return MOTOR_ERR_NOT_READY;
    }

    MotorResultCode rc;
    if (motors_status_is_error(motors_state.status)) {
        /* ERROR already closed admission via motors_enter_error_state(). */
        rc = MOTOR_ERR_HARDWARE_ERROR;
    } else if (aborted) {
        /* The drain failed (position uncertain).  The executor already closed
         * admission at the point it published the failure, so nothing more to do
         * here — just report the failure. */
        rc = MOTOR_ERR_NOT_READY;
    } else {
        rc = MOTOR_OK;
        motors_test_hook_after_move_axis_stop_open();
    }

    if (rc == MOTOR_OK && restore != TRACKING_NONE) {
        motors_test_hook_before_move_axis_tracking_restore();
        rc = motors_restore_tracking_if_idle(restore);
    }
    return rc;
}

MotorResultCode motors_stop(void) {
    if (!motors_control_lock()) {
        return MOTOR_ERR_HARDWARE_ERROR;
    }

    MotorResultCode rc = motors_stop_and_wait();
    if (rc != MOTOR_OK) {
        motors_control_unlock();
        return rc;
    }

    /* Publish READY and read the final status in one critical section, so a
     * fault that lands after the drain result but before the publish is still
     * reported as an error, never as a clean stop.  STOP must not unpark: only
     * motors_unpark() exits PARKED. */
    portENTER_CRITICAL(&motors_state_lock);
    if (motors_state.status != MOTORS_STATUS_PARKED
        && motors_state.status != MOTORS_STATUS_ERROR) {
        motors_state.status = MOTORS_STATUS_READY;
    }
    motors_state.tracking = TRACKING_NONE;
    motors_state.guiding = false;
    rc = motors_status_is_error(motors_state.status)
         ? MOTOR_ERR_HARDWARE_ERROR : MOTOR_OK;
    portEXIT_CRITICAL(&motors_state_lock);

    /* Reopen admission only when no ERROR is latched.  A fault that landed after
     * the status check above keeps admission closed and is reported here. */
    if (!motors_admission_open()) {
        rc = MOTOR_ERR_HARDWARE_ERROR;
    }
    motors_control_unlock();
    return rc;
}
