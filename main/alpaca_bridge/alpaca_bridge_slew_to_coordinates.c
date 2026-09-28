/* Alpaca bridge — slew / abort
 *
 * Thin wrappers that map Alpaca coordinate calls directly to the
 * mount public API. RA is in hours, DEC is in degrees.
 */

#include "alpaca_bridge.h"

#include <math.h>

#include "mount.h"
#include "mount_internal.h"

MountResult alpaca_bridge_slew_to_coordinates(float ra_hours, float dec_deg) {
    /* Validate the celestial target before touching the mount.  A raw
     * equatorial_to_axis() would wrap an out-of-range RA (25h -> 1h) and slew
     * silently to the wrong sky position. */
    if (!isfinite(ra_hours) || ra_hours < 0.0f || ra_hours >= 24.0f) {
        return mount_result_error("RightAscension out of range [0,24)");
    }
    if (!isfinite(dec_deg) || dec_deg < -90.0f || dec_deg > 90.0f) {
        return mount_result_error("Declination out of range [-90,90]");
    }
    return mount_slew_to_coordinates(ra_hours, dec_deg, 4);
}

MountResult alpaca_bridge_abort_slew(void) {
    /* AbortSlew stops the in-progress slew, not an active tracking.  Keep a
     * currently-active tracking mode instead of clearing it. */
    MotorsState s = motors_current_state();
    TrackingMode saved = s.tracking;
    MountResult r = mount_stop();
    if (r.ok && saved != TRACKING_NONE) {
        mount_set_tracking(saved);
    }
    return r;
}
