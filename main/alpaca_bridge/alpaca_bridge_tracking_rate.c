/* Alpaca bridge — tracking rate
 *
 * Maps between the mount's internal TrackingMode enum and the
 * ASCOM DriveRates integer values (0 = sidereal, 1 = lunar,
 * 2 = solar, 3 = king — not implemented).
 */

#include "alpaca_bridge.h"
#include "alpaca_bridge_internal.h"

#include <stdio.h>

#include "mount.h"
#include "mount_internal.h"
#include "motors/motors.h"

/* Map an ASCOM DriveRates value to the internal tracking mode. */
static TrackingMode rate_to_mode(int rate) {
    switch (rate) {
        case 1:  return TRACKING_LUNAR;
        case 2:  return TRACKING_SOLAR;
        case 0:
        default: return TRACKING_SIDEREAL;
    }
}

/*
 * Return the currently selected tracking rate as an ASCOM DriveRates value.
 * This is the setpoint (what TrackingRate was last set to), not the active
 * mode: when tracking is off the mount still reports the selected rate, so
 * N.I.N.A. can read back the user's choice.
 */
int alpaca_bridge_get_tracking_rate(void) {
    return alpaca_bridge_state.selected_tracking_rate;
}

/*
 * True if any automatic tracking mode is active (sidereal, lunar, or solar).
 * Manual and None are considered "not tracking".
 */
bool alpaca_bridge_get_tracking(void) {
    MotorsState s = motors_current_state();
    if (motors_status_is_error(s.status)) return false;
    return s.tracking != TRACKING_NONE;
}

/*
 * Enable or disable tracking without changing the current rate.
 * When enabling, the previously-selected rate (via TrackingRate) is used,
 * so a lunar/solar selection survives a Tracking off/on cycle.
 */
MountResult alpaca_bridge_set_tracking(bool enabled) {
    if (!enabled)
        return mount_set_tracking(TRACKING_NONE);
    return mount_set_tracking(rate_to_mode(alpaca_bridge_state.selected_tracking_rate));
}

/*
 * Set tracking to a specific ASCOM DriveRates value.
 * This is a setpoint: it stores the rate and, only if tracking is already
 * active, applies it immediately — it does not start tracking on its own.
 */
MountResult alpaca_bridge_set_tracking_rate(int rate) {
    if (rate < 0 || rate > 2) {
        return mount_result_error("Tracking rate out of range [0..2]");
    }
    alpaca_bridge_state.selected_tracking_rate = rate;
    MotorsState s = motors_current_state();
    if (s.tracking != TRACKING_NONE) {
        return mount_set_tracking(rate_to_mode(rate));
    }
    return mount_result_ok();
}

/*
 * Fill `buf` with the ASCOM name for tracking rate `idx`
 * (0 = driveSidereal, 1 = driveLunar, 2 = driveSolar).
 */
void alpaca_bridge_get_tracking_rate_name(int idx, char *buf, size_t len) {
    switch (idx) {
        case 0: snprintf(buf, len, "driveSidereal");
            break;
        case 1: snprintf(buf, len, "driveLunar");
            break;
        case 2: snprintf(buf, len, "driveSolar");
            break;
        default: buf[0] = '\0';
            break;
    }
}
