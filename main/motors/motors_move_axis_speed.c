/* Motors - motors_move_axis_speed.c
 *
 * Purpose: request continuous single-axis speed motion.
 * Positive rate = forward, negative = reverse, zero = stop that axis.
 * Used by MoveAxis, joystick, and guiding.
 *
 * Speeds are clamped to the profile's manual ceiling (max_manual_dps).
 *
 * The manual session is owned here: a non-zero rate opens it (saving the tracking
 * mode to restore), a zero closes it (draining the MoveAxis and restoring the
 * saved tracking), and STOP/PARK invalidate it.
 */
#include "motors.h"
#include "motors_internal.h"

#include <math.h>

/*
 * Test hooks — weak no-ops overridden by the host harness to pause the producer
 * deterministically at the two edges of the manual-session publication window.
 * Production builds link the default (no-op) definitions.
 */
void __attribute__((weak)) motors_test_hook_before_move_axis_enqueue(void) {}
void __attribute__((weak)) motors_test_hook_after_move_axis_enqueue(void) {}
void __attribute__((weak)) motors_test_hook_after_move_axis_stop_open(void) {}
void __attribute__((weak)) motors_test_hook_before_move_axis_tracking_transition(void) {}
void __attribute__((weak)) motors_test_hook_before_move_axis_tracking_restore(void) {}

/* MoveAxis setpoint — the authoritative continuous-move rate.  Writing it
 * bumps the version, which the motion task watches to pick up the newest rate
 * without relying on the (FIFO) command queue. */
volatile float motors_move_axis_ra_dps = 0.0f;
volatile float motors_move_axis_dec_dps = 0.0f;
volatile uint32_t motors_move_axis_version = 0;

/* Manual-session state — owned by motors_admission_lock (see motors_internal.h). */
volatile bool motors_manual_move = false;
TrackingMode motors_saved_tracking = TRACKING_NONE;
volatile uint32_t motors_manual_session = 0;

MotorResultCode motors_set_move_axis_speed(float ra_speed, float dec_speed) {
    if (motors_status_is_error(motors_state.status)) {
        return MOTOR_ERR_HARDWARE_ERROR;
    }

    /* Defense in depth: the protocol parser already rejects non-finite floats,
     * but a NaN rate would silently slip past the clamp below. */
    if (!isfinite(ra_speed) || !isfinite(dec_speed)) {
        return MOTOR_ERR_OUT_OF_RANGE;
    }

    if (ra_speed == 0.0f && dec_speed == 0.0f) {
        return motors_move_axis_stop();
    }

    if (motors_status_is_parked(motors_state.status)) {
        return MOTOR_ERR_PARKED;
    }

    /* Clamp to the manual move-axis ceiling. */
    float max_dps = mount_profile.max_manual_dps;
    if (ra_speed > max_dps) ra_speed = max_dps;
    if (ra_speed < -max_dps) ra_speed = -max_dps;
    if (dec_speed > max_dps) dec_speed = max_dps;
    if (dec_speed < -max_dps) dec_speed = -max_dps;

    /* Test hook: pause here before the enqueue, so a test can observe that a
     * not-yet-admitted MoveAxis has not touched the active motion. */
    motors_test_hook_before_move_axis_enqueue();

    MotionCommand cmd = {
        .type = MOTION_CMD_MOVE_AXIS,
        .ra_target_deg = 0.0f, /* set by the task from limits */
        .dec_target_deg = 0.0f,
        .ra_speed = ra_speed,
        .dec_speed = dec_speed,
        .tracking_mode = TRACKING_NONE,
    };
    /* Enqueue + publish setpoint + open the session as one admission
     * transaction.  The tracking mode to restore is captured by the motion task
     * at the transition (not here, before admission), so a rejected order or a
     * STOP in between never leaves a stale save. */
    if (!motors_queue_put_move_axis(&cmd, ra_speed, dec_speed)) {
        return MOTOR_ERR_BUSY;
    }

    /* Test hook: the order was accepted.  Pause here so a test can exercise a
     * STOP that invalidates the session after acceptance — the producer no
     * longer writes tracking afterwards, so it cannot cancel new tracking. */
    motors_test_hook_before_move_axis_tracking_transition();

    motors_test_hook_after_move_axis_enqueue();

    return MOTOR_OK;
}

/*
 * Close the manual session: drain the MoveAxis (a queued or running one), publish
 * READY, and restore the tracking mode the session saved — all under the control
 * mutex, so a concurrent STOP/PARK cannot cancel the tracking this operation
 * resumes.  A redundant zero (no manual session) is a no-op and does not cancel
 * tracking, guiding or a goto.
 */
MotorResultCode motors_move_axis_stop(void) {
    if (motors_status_is_error(motors_state.status)) {
        return MOTOR_ERR_HARDWARE_ERROR;
    }
    if (!motors_control_lock()) {
        return MOTOR_ERR_HARDWARE_ERROR;
    }

    uint32_t session;
    TrackingMode restore;
    bool manual;
    portENTER_CRITICAL(&motors_admission_lock);
    manual = motors_manual_move;
    if (manual) {
        motors_manual_invalidate_locked(&session, &restore);
    }
    portEXIT_CRITICAL(&motors_admission_lock);

    if (!manual) {
        /* No manual session: a redundant zero.  Do not touch any other motion. */
        motors_control_unlock();
        return MOTOR_OK;
    }

    MotorResultCode rc = motors_manual_finish(session, restore);
    motors_control_unlock();
    return rc;
}

void motors_get_move_axis_setpoint(float *ra_dps, float *dec_dps) {
    if (ra_dps == NULL || dec_dps == NULL) {
        return;
    }
    portENTER_CRITICAL(&motors_admission_lock);
    *ra_dps = motors_move_axis_ra_dps;
    *dec_dps = motors_move_axis_dec_dps;
    portEXIT_CRITICAL(&motors_admission_lock);
}

/*
 * Move a single axis (0 = RA, 1 = DEC) continuously at `rate` deg/s, preserving
 * the other axis's current rate.  The read of the other axis, the modification
 * and the publication happen as one atomic admission transaction, so concurrent
 * single-axis updates cannot lose each other's rate.
 *
 * A non-zero rate always keeps at least one axis moving and is a pure setpoint
 * update.  A zero rate that leaves both axes at zero finalizes the manual
 * session (reusing the E04 localized finalization), so it is serialized against
 * STOP/PARK with the control mutex.
 */
MotorResultCode motors_set_move_axis_rate(int axis, float rate) {
    if (motors_status_is_error(motors_state.status)) {
        return MOTOR_ERR_HARDWARE_ERROR;
    }
    if (axis != 0 && axis != 1) {
        return MOTOR_ERR_INVALID_AXIS;
    }
    if (!isfinite(rate)) {
        return MOTOR_ERR_OUT_OF_RANGE;
    }
    if (motors_status_is_parked(motors_state.status)) {
        return MOTOR_ERR_PARKED;
    }

    float max_dps = mount_profile.max_manual_dps;
    if (rate > max_dps) rate = max_dps;
    if (rate < -max_dps) rate = -max_dps;

    motors_test_hook_before_move_axis_enqueue();

    if (rate != 0.0f) {
        /* Pure setpoint update: read the other axis, apply this axis, and
         * enqueue+publish in one critical section.  When no manual session is
         * live, the other axis's stale setpoint is meaningless (e.g. after a
         * STOP it was never cleared), so it is treated as zero: a fresh session
         * starts from scratch and never resurrects pre-STOP work. */
        MotionCommand cmd = {
            .type = MOTION_CMD_MOVE_AXIS,
            .ra_target_deg = 0.0f,
            .dec_target_deg = 0.0f,
            .tracking_mode = TRACKING_NONE,
        };
        bool ok;
        portENTER_CRITICAL(&motors_admission_lock);
        {
            float other = (axis == 0) ? motors_move_axis_dec_dps
                                      : motors_move_axis_ra_dps;
            if (!motors_manual_move) {
                other = 0.0f;
            }
            float ra_new = (axis == 0) ? rate : other;
            float dec_new = (axis == 1) ? rate : other;
            ok = motors_queue_put_move_axis_under_lock(&cmd, ra_new, dec_new);
        }
        portEXIT_CRITICAL(&motors_admission_lock);
        if (!ok) {
            return MOTOR_ERR_BUSY;
        }
        motors_test_hook_before_move_axis_tracking_transition();
        motors_test_hook_after_move_axis_enqueue();
        return MOTOR_OK;
    }

    /* rate == 0: zeroing one axis.  Whether the session must finalize depends
     * on the other axis's live rate, so serialize the whole decision+drain
     * against STOP/PARK with the control mutex. */
    if (!motors_control_lock()) {
        return MOTOR_ERR_HARDWARE_ERROR;
    }

    bool finalize = false;
    uint32_t session = 0;
    TrackingMode restore = TRACKING_NONE;
    bool enqueued = false;

    portENTER_CRITICAL(&motors_admission_lock);
    {
        /* When no manual session is live, the other axis's stale setpoint is
         * meaningless (it was never cleared by a STOP), so treat it as zero. */
        bool has_session = motors_manual_move;
        float other = (axis == 0) ? motors_move_axis_dec_dps
                                  : motors_move_axis_ra_dps;
        if (!has_session) {
            other = 0.0f;
        }
        float ra_new = (axis == 0) ? 0.0f : other;
        float dec_new = (axis == 1) ? 0.0f : other;

        if (ra_new == 0.0f && dec_new == 0.0f) {
            /* Both axes at zero.  The read-modify-write and the invalidation are
             * one critical section, so a concurrent other-axis update either
             * lands before this (and is seen as non-zero) or after (and opens a
             * fresh session). */
            if (!has_session) {
                /* No live session: a redundant zero must not touch anything. */
                portEXIT_CRITICAL(&motors_admission_lock);
                motors_control_unlock();
                return MOTOR_OK;
            }
            finalize = true;
            motors_manual_invalidate_locked(&session, &restore);
        } else {
            MotionCommand cmd = {
                .type = MOTION_CMD_MOVE_AXIS,
                .ra_target_deg = 0.0f,
                .dec_target_deg = 0.0f,
                .tracking_mode = TRACKING_NONE,
            };
            enqueued = motors_queue_put_move_axis_under_lock(&cmd, ra_new, dec_new);
        }
    }
    portEXIT_CRITICAL(&motors_admission_lock);

    MotorResultCode rc;
    if (finalize) {
        rc = motors_manual_finish(session, restore);
    } else if (!enqueued) {
        rc = MOTOR_ERR_BUSY;
    } else {
        rc = MOTOR_OK;
        motors_test_hook_before_move_axis_tracking_transition();
        motors_test_hook_after_move_axis_enqueue();
    }

    motors_control_unlock();
    return rc;
}

