/* Alpaca bridge — alpaca_bridge_state.c
 *
 * Shared mutable state for the Alpaca bridge layer.
 * Holds transient values that Alpaca clients can read and write.
 */

#include "alpaca_bridge_internal.h"

AlpacaBridgeState alpaca_bridge_state = {
    .target_ra = 0.0f,
    .target_dec = 0.0f,
    .selected_tracking_rate = 0, /* sidereal */
    .connected = false,
};

bool alpaca_bridge_get_connected(void) {
    return alpaca_bridge_state.connected;
}

void alpaca_bridge_set_connected(bool connected) {
    alpaca_bridge_state.connected = connected;
}
