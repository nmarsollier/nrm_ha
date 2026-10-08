#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

/* ═══════════════════════════════════════════════════════════════
 * Alpaca Bridge — shared mutable state
 *
 * Holds transient values that Alpaca clients can read and write
 * (target coordinates).  These do not need NVS persistence — they
 * reset on reboot.
 * ═══════════════════════════════════════════════════════════════ */

typedef struct {
    float target_ra; /* Target right ascension (hours) */
    float target_dec; /* Target declination (degrees) */
    int selected_tracking_rate; /* ASCOM DriveRates value set via TrackingRate (0/1/2) */
    bool connected; /* Logical client link (ASCOM Connected) */
} AlpacaBridgeState;

extern AlpacaBridgeState alpaca_bridge_state;

/* ═══════════════════════════════════════════════════════════════
 * Astronomical helpers (shared by altitude, azimuth, sidereal time)
 * ═══════════════════════════════════════════════════════════════ */

/*
 * Convert a Unix timestamp to Julian Date.
 */
double alpaca_bridge_unix_to_jd(time_t t);

/*
 * Compute Greenwich Mean Sidereal Time (GMST) in hours from a Julian Date.
 * Uses the standard USNO approximation valid to ~1 arcsecond.
 */
double alpaca_bridge_gmst_hours(double jd);

/*
 * Compute horizontal coordinates (altitude, azimuth) from the given
 * equatorial position and current site/UTC time.  Coordinates are passed in
 * so a caller can supply a coherent snapshot instead of re-reading the mount.
 * Either output pointer may be NULL to skip that value.
 */
void alpaca_bridge_horizontal(float ra_hours, float dec_deg,
                              float *alt_deg, float *az_deg);
