/* Motors - motors_get_slewing_speed.c
 *
 * Purpose: return the slewing angular speed for a speed rate profile.
 */
#include "motors.h"
#include "motors_internal.h"

/*
 * Map a speed_rate profile (1..4, higher = faster) to the axis angular speed in
 * degrees per second.  The intermediate rates are fixed fractions of the
 * profile's max_slew_dps, so they scale with the mount (NRM 6 °/s → 1, 3, 4.5;
 * BFT 4 °/s → ~0.667, 2, 3).
 */
float motors_get_slewing_speed(int speed_rate) {
    float max_dps = mount_profile.max_slew_dps;
    switch (speed_rate) {
        case 1: return max_dps * (1.0f / 6.0f);
        case 2: return max_dps * 0.5f;
        case 3: return max_dps * 0.75f;
        default: return max_dps;
    }
}
