/* Alpaca bridge — destination pier side
 *
 * Returns the pier side the mount WOULD end up on after a slew to the given
 * equatorial coordinates.
 *
 * The result must match what a real GOTO would do, so it uses the same planner
 * as equatorial_to_axis() — which picks the valid mechanical solution with the
 * lowest movement cost — rather than a simplified hour-angle-sign heuristic.
 */

#include "alpaca_bridge.h"
#include "alpaca_bridge_internal.h"

#include "mount.h"
#include <time.h>

int alpaca_bridge_get_destination_side_of_pier(float ra_hours, float dec_deg) {
    EquatorialCoordinates eq = { .ra_hours = ra_hours, .dec_deg = dec_deg };
    AxisCoordinates current = { .ra_axis_deg = motors_get_ra_deg(),
                                .dec_axis_deg = motors_get_dec_deg() };
    AxisCoordinates out;

    if (equatorial_to_axis(eq, current, time(NULL), &out)) {
        return out.pier_side;
    }

    /* Target unreachable within limits — no meaningful destination side. */
    return alpaca_bridge_get_side_of_pier();
}
