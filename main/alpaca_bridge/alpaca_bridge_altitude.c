/* Alpaca bridge — altitude / azimuth
 *
 * Calculate horizontal coordinates (altitude, azimuth) from the current
 * equatorial position, site location, and UTC time.
 */

#include "alpaca_bridge.h"
#include "alpaca_bridge_internal.h"

#include <math.h>
#include <time.h>

#include "mount.h"
#include "mount_internal.h"

/* ─── Astronomical helpers ─── */

/*
 * Convert a Unix timestamp to Julian Date.
 * Used as input to the GMST and alt/az calculations.
 */
double alpaca_bridge_unix_to_jd(time_t t) {
    return (double) t / 86400.0 + 2440587.5;
}

/*
 * Compute Greenwich Mean Sidereal Time (GMST) in hours from a Julian Date.
 * Uses the standard USNO approximation valid to ~1 arcsecond.
 */
double alpaca_bridge_gmst_hours(double jd) {
    double T = (jd - 2451545.0) / 36525.0;
    double gmst = 280.46061837 + 360.98564736629 * (jd - 2451545.0)
                  + 0.000387933 * T * T - (T * T * T) / 38710000.0;
    gmst = fmod(gmst, 360.0);
    if (gmst < 0.0) gmst += 360.0;
    return gmst / 15.0;
}

/* ─── Horizontal coordinates ─── */

/*
 * Compute horizontal coordinates (altitude, azimuth) from the given
 * equatorial position and current site/UTC time.  Coordinates are passed in
 * so a caller can supply a coherent snapshot instead of re-reading the mount.
 * Either output pointer may be NULL to skip that value.
 */
void alpaca_bridge_horizontal(float ra_hours, float dec_deg,
                              float *alt_deg, float *az_deg) {
    time_t now = time(NULL);
    double jd = alpaca_bridge_unix_to_jd(now);
    double gmst_h = alpaca_bridge_gmst_hours(jd);
    double lst_h = gmst_h + (double) mount_internal_state.lon / 15.0;

    double ha_rad = (lst_h - (double) ra_hours) * 15.0 * M_PI / 180.0;
    double dec_rad = (double) dec_deg * M_PI / 180.0;
    double lat_rad = (double) mount_internal_state.lat * M_PI / 180.0;

    double alt_rad = asin(sin(dec_rad) * sin(lat_rad)
                          + cos(dec_rad) * cos(lat_rad) * cos(ha_rad));

    if (alt_deg) {
        *alt_deg = (float) (alt_rad * 180.0 / M_PI);
    }

    if (az_deg) {
        double cos_az = (sin(dec_rad) - sin(alt_rad) * sin(lat_rad))
                        / (cos(alt_rad) * cos(lat_rad));
        if (cos_az > 1.0) cos_az = 1.0;
        if (cos_az < -1.0) cos_az = -1.0;

        double az_rad = acos(cos_az);
        if (sin(ha_rad) > 0.0) az_rad = 2.0 * M_PI - az_rad;
        *az_deg = (float) (az_rad * 180.0 / M_PI);
    }
}

/*
 * Current telescope altitude above the horizon (degrees).
 * 90° = zenith, 0° = horizon, negative = below horizon.
 */
float alpaca_bridge_get_altitude(void) {
    float alt = 0.0f;
    alpaca_bridge_horizontal(mount_get_ra_hours(), mount_get_dec_deg(),
                             &alt, NULL);
    return alt;
}

/*
 * Current telescope azimuth (degrees, 0° = North, 90° = East).
 */
float alpaca_bridge_get_azimuth(void) {
    float az = 0.0f;
    alpaca_bridge_horizontal(mount_get_ra_hours(), mount_get_dec_deg(),
                             NULL, &az);
    return az;
}
