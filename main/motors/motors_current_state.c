/* Motors - motors_current_state.c
 *
 * Purpose: return a copy of the authoritative motors state,
 * and provide degree-format accessors for external consumers.
 */
#include "motors.h"
#include "motors_internal.h"

/*
 * Return a snapshot copy of the motors module's authoritative state.
 * External consumers should use this for status and telemetry reads.
 *
 * The whole copy is taken under motors_state_lock so every writer to
 * motors_state must also take that lock — otherwise a reader on another core
 * could observe a torn int64_t counter or a half-updated multi-field state
 * transition.
 */
MotorsState motors_current_state(void) {
    MotorsState copy;
    copy = motors_state;
    return copy;
}

float motors_get_ra_deg(void) {
    int64_t steps;
    steps = motors_state.ra_steps;
    return motors_steps_to_deg(steps);
}

float motors_get_dec_deg(void) {
    int64_t steps;
    steps = motors_state.dec_steps;
    return motors_steps_to_deg(steps);
}
