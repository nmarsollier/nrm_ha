/* Alpaca bridge — target coordinates
 *
 * Stored in AlpacaBridgeState. The Alpaca protocol separates "set target"
 * from "slew to target" — clients set TargetRightAscension /
 * TargetDeclination first, then call slewtotarget / synctotarget.
 */

#include "alpaca_bridge.h"
#include "alpaca_bridge_internal.h"

#include <math.h>

#include "mount.h"
#include "mount_internal.h"

float alpaca_bridge_get_target_ra(void) { return alpaca_bridge_state.target_ra; }
float alpaca_bridge_get_target_dec(void) { return alpaca_bridge_state.target_dec; }

MountResult alpaca_bridge_set_target_ra(float ra_hours) {
    if (!isfinite(ra_hours) || ra_hours < 0.0f || ra_hours >= 24.0f) {
        return mount_result_error("TargetRightAscension out of range [0,24)");
    }
    alpaca_bridge_state.target_ra = ra_hours;
    return mount_result_ok();
}

MountResult alpaca_bridge_set_target_dec(float dec_deg) {
    if (!isfinite(dec_deg) || dec_deg < -90.0f || dec_deg > 90.0f) {
        return mount_result_error("TargetDeclination out of range [-90,90]");
    }
    alpaca_bridge_state.target_dec = dec_deg;
    return mount_result_ok();
}

MountResult alpaca_bridge_slew_to_target(void) {
    if (alpaca_bridge_state.target_ra == 0.0f && alpaca_bridge_state.target_dec == 0.0f)
        return mount_result_error("Target not set");
    return mount_slew_to_coordinates(alpaca_bridge_state.target_ra, alpaca_bridge_state.target_dec, 2);
}
