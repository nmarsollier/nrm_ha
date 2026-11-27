/* Motors - motors_get_move_axis_speed.c
 *
 * Purpose: return the manual move-axis angular speed for a speed rate profile.
 */
#include "motors.h"
#include "motors_internal.h"

/*
 * Map a speed_rate profile (1..10, higher = faster) to the continuous
 * MoveAxis / joystick speed in degrees per second.  MoveAxis does not ramp
 * acceleration, so this scale is geometric and gentle: rate 1 sits near the
 * sidereal rate for fine manual centering, rate 10 is the manual ceiling
 * (the profile's max_manual_dps).  The ratio between consecutive rates is
 * 200^(1/9) ~= 1.8; each rate is a fixed fraction of max_manual_dps.
 */
float motors_get_move_axis_speed(int speed_rate) {
    float max_dps = mount_profile.max_manual_dps;
    switch (speed_rate) {
        case 1: return max_dps * 0.005f;
        case 2: return max_dps * 0.009f;
        case 3: return max_dps * 0.016f;
        case 4: return max_dps * 0.029f;
        case 5: return max_dps * 0.053f;
        case 6: return max_dps * 0.095f;
        case 7: return max_dps * 0.171f;
        case 8: return max_dps * 0.308f;
        case 9: return max_dps * 0.555f;
        default: return max_dps;
    }
}
