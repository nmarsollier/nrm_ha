/* Alpaca bridge — DeviceState coherent snapshot
 *
 * Reads the authoritative MotorsState once and derives every operational
 * Telescope property from that single read, so the DeviceState endpoint never
 * exposes a mix of two different instants.
 *
 * RA/DEC are the *equatorial* coordinates — the same axis→equatorial
 * conversion the rightascension/declination endpoints use — while pier side
 * and "at home" come from the raw axis position (they depend on the axis
 * sign/magnitude, not the equatorial projection).
 */
#include "alpaca_bridge.h"
#include "alpaca_bridge_internal.h"

#include <math.h>

#include "mount.h"
#include "motors/motors.h"

void alpaca_bridge_get_device_state(AlpacaDeviceState *out) {
    MotorsState s = motors_current_state();

    AxisCoordinates axis = {
        .ra_axis_deg = motors_get_ra_deg(),
        .dec_axis_deg = motors_get_dec_deg(),
    };
    EquatorialCoordinates eq = axis_to_equatorial(axis);

    out->right_ascension = eq.ra_hours;
    out->declination = eq.dec_deg;
    out->side_of_pier = (axis.dec_axis_deg >= 0.0f) ? 0 : 1;
    out->is_pulse_guiding = s.guiding;
    out->slewing = s.status == MOTORS_STATUS_SLEWING;
    out->tracking = s.status == MOTORS_STATUS_TRACKING;
    out->at_park = s.status == MOTORS_STATUS_PARKED;
    out->at_home = s.status == MOTORS_STATUS_READY &&
                   fabsf(axis.ra_axis_deg) < 1.0f && fabsf(axis.dec_axis_deg) < 1.0f;

    alpaca_bridge_horizontal(eq.ra_hours, eq.dec_deg,
                             &out->altitude, &out->azimuth);
}
