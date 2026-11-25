/* Geometry — geometry_common.c — shared time helpers.
 *
 * The Julian Date / GMST / LST math is shared by every geometry implementation
 * (German and fork).  Pure functions of the fractional unix instant and the
 * site longitude.  The slew-ramp math lives in main/motion/slew_ramp.c.
 */
#include "mount_geometry.h"

#include <math.h>

double geometry_gmst_hours(double unix_seconds) {
    double jd = unix_seconds / 86400.0 + 2440587.5;
    double T = (jd - 2451545.0) / 36525.0;
    double gmst_deg = 280.46061837 + 360.98564736629 * (jd - 2451545.0)
                      + 0.000387933 * T * T - (T * T * T) / 38710000.0;
    gmst_deg = fmod(gmst_deg, 360.0);
    if (gmst_deg < 0.0) gmst_deg += 360.0;
    return gmst_deg / 15.0;
}

float geometry_lst(float lon_deg, double unix_seconds) {
    /* Normalise to [0, 24).  fmod keeps this O(1) and cannot spin forever on a
     * non-finite longitude read from a corrupted settings store. */
    double lst_h = geometry_gmst_hours(unix_seconds) + (lon_deg / 15.0);
    lst_h = fmod(lst_h, 24.0);
    if (lst_h < 0.0) lst_h += 24.0;
    return (float) lst_h;
}
