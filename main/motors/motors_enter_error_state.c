/* Motors - motors_enter_error_state.c
 *
 * Purpose: single entry point to put the motors subsystem into the
 * unrecoverable ERROR state, and track the fault cause so a power-loss
 * recovery cannot silently clear a hardware fault.
 *
 * Any in-flight RMT transmission is stopped by motors_rmt_reset_both() when
 * the motion loop exits; this function only flags the state.
 *
 * Once in ERROR, the motors module rejects all movement commands.
 * A hardware fault is latched and only a full reboot clears it; a power-loss
 * fault is recoverable when the 12 V rail returns.
 */
#include "motors_internal.h"

/* Latched hardware fault — set on GPIO/RMT init failure, never cleared. */
static bool s_hardware_fault = false;

/* Publish ERROR and close admission, assuming motors_state_lock is held. */
static void enter_error_locked(void) {
    motors_state.status = MOTORS_STATUS_ERROR;
    motors_state.tracking = TRACKING_NONE;
    motors_state.guiding = false;
    motors_admission_close();
}

void motors_enter_error_state(void) {
    /* Publish ERROR and, in the same state-lock window, close admission and
     * invalidate queued + manual work.  The nested lock order is
     * motors_state_lock → motors_admission_lock (matching motors_admission_open),
     * so the ERROR publication and the admission reopen are linearised against
     * each other — no producer can admit work after the fault is latched. */
    portENTER_CRITICAL(&motors_state_lock);
    enter_error_locked();
    portEXIT_CRITICAL(&motors_state_lock);
}

void motors_enter_hardware_fault(void) {
    /* Latch the fault and publish ERROR in the same state-lock window, so a
     * concurrent reader can never observe the fault flag without the ERROR
     * status (or vice versa). */
    portENTER_CRITICAL(&motors_state_lock);
    s_hardware_fault = true;
    enter_error_locked();
    portEXIT_CRITICAL(&motors_state_lock);
}

bool motors_has_hardware_fault(void) {
    bool fault;
    portENTER_CRITICAL(&motors_state_lock);
    fault = s_hardware_fault;
    portEXIT_CRITICAL(&motors_state_lock);
    return fault;
}

/*
 * Recover from a non-hardware ERROR (e.g. a power loss) back to READY and reopen
 * admission, but only if no hardware fault is latched.  The READY publication and
 * the admission reopen happen in the same state-lock window (state→admission), so
 * a concurrent motors_enter_hardware_fault() cannot slip between them; the caller
 * holds the control mutex so a STOP/PARK cannot begin between READY and reopen.
 */
void motors_recover_to_ready(void) {
    portENTER_CRITICAL(&motors_state_lock);
    if (!s_hardware_fault) {
        motors_state.status = MOTORS_STATUS_READY;
        motors_state.tracking = TRACKING_NONE;
        motors_admission_open_locked();
    }
    portEXIT_CRITICAL(&motors_state_lock);
}

/*
 * Clear the latched hardware fault.  In production a hardware fault is only
 * cleared by a full reboot; this reset exists solely for the host test harness,
 * which reuses one process across many tests and must start each from a clean
 * fault latch.
 */
void motors_reset_hardware_fault(void) {
    s_hardware_fault = false;
}
