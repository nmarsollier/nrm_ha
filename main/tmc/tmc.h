/*
 * tmc.h — TMC2209 driver public API (two axes: RA and DEC).
 *
 * The coil currents (IRUN/IHOLD) live in the selected mount profile
 * (mount_profile.hardware).  The initial microstep resolution is supplied by the
 * caller (the motors layer, from the selected mount profile): the driver applies
 * one resolution to both axes.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

/*
 * Initialize the two single-wire UARTs, configure GCONF/IHOLD_IRUN/CHOPCONF
 * on both drivers and verify that each latched the requested microsteps.
 *
 * `microsteps` is the initial resolution applied to both axes (the profile's
 * canonical/tracking resolution).  The drivers are always enabled (no EN pin is
 * connected).  Must be called before any step/direction motion is started.
 */
esp_err_t tmc2209_hw_init(uint16_t microsteps);

/*
 * Probe both drivers over UART: return true only when both answer a register
 * read.  This doubles as the "TMC working" sense — the drivers are fed by VM
 * (12 V), so they are silent when the rail is absent and their logic supply
 * collapses.  Both motors must be alive for the mount to move.
 */
bool tmc2209_probe(void);

/*
 * Re-configure the driver registers after they stop answering and then answer
 * again (power returned): their registers were lost while unpowered, so they
 * are rewritten at `microsteps` (the profile's canonical resolution).  Does not
 * re-install the UARTs.
 */
esp_err_t tmc2209_reinit(uint16_t microsteps);

/*
 * Change the microstep resolution on both drivers at runtime: read-modify-write
 * of CHOPCONF.MRES (preserving the other fields), verified by read-back with
 * retries.  Returns ESP_OK only when BOTH drivers latched the requested value.
 * Must be called with the mount otherwise idle (no STEP stream in flight).
 */
esp_err_t tmc2209_set_microsteps(uint16_t microsteps);

/*
 * Current active microstep resolution on both drivers.  The value is only ever
 * recorded after a verified read-back, so it is always the real one — the motors
 * layer trusts it for step accounting and status reporting.
 */
uint16_t tmc2209_get_microsteps(void);

/* Chopper mode selected per motion strategy (see motors_microsteps.c). */
typedef enum {
    TMC_CHOP_STEALTH = 0,   /* silent PWM control — tracking, guiding, gentle slews */
    TMC_CHOP_SPREAD,        /* SpreadCycle chopper — coarse slews */
} TmcChopMode;

/*
 * Change the chopper mode on both drivers at runtime: read-modify-write of
 * GCONF.en_SpreadCycle (accounting for the SPREAD pin inversion), verified by
 * read-back with retries.  Returns ESP_OK only when BOTH drivers latched the
 * requested mode.  Must be called with the mount otherwise idle (no STEP stream).
 */
esp_err_t tmc2209_set_chop_mode(TmcChopMode mode);

/*
 * Current active chopper mode on both drivers.  The value is only ever recorded
 * after a verified read-back, so it is always the real one.
 */
TmcChopMode tmc2209_get_chop_mode(void);

/*
 * Query whether both TMC2209 UARTs and drivers were initialised successfully.
 * Used for error-state LED signalling.
 */
bool tmc2209_is_initialized(void);
