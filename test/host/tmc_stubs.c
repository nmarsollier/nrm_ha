/* tmc_stubs.c — host stub for the TMC2209 runtime microstep switch.
 *
 * The host regression compiles the REAL motors_microsteps.c and motors_power.c,
 * which reach into the TMC layer to change CHOPCONF.MRES, to change
 * GCONF.en_SpreadCycle (chopper mode), to query the active values, and to
 * re-initialise the drivers after a power loss.  The single-wire UART driver is
 * hardware-only, so this stub models the state the real driver keeps: the
 * setters record the value, the getters return it.  The tests exercise the
 * selection/accounting logic, not the wire protocol (covered by the proto
 * acceptance suite on real hardware).
 */
#include "tmc.h"

/* The real driver starts unconfigured and is set to the profile's canonical
 * resolution by tmc2209_hw_init(); the host harness skips that hardware step,
 * so the stub starts pre-configured at the canonical tracking resolution (64
 * for both profiles). */
static uint16_t s_microsteps = 64;
static TmcChopMode s_chop_mode = TMC_CHOP_STEALTH;
static bool s_reinit_fail = false;
static bool s_microsteps_fail = false;

/* Test-only: make tmc2209_reinit fail to exercise the hardware-fault recovery. */
void host_tmc_set_reinit_fail(int fail) { s_reinit_fail = (fail != 0); }

/* Test-only: force the cached microstep resolution to an arbitrary value, so a
 * test can exercise the "resolution unverified (0)" guard in
 * motors_microsteps_prepare — the state the real driver holds at boot before
 * tmc2209_hw_init() verifies a resolution. */
void host_tmc_set_microsteps(uint16_t microsteps) { s_microsteps = microsteps; }

/* Test-only: make tmc2209_set_microsteps fail on BOTH axes, so a test can
 * exercise the "switch failed → stop + ERROR" path of apply_microsteps(). */
void host_tmc_set_microsteps_fail(int fail) { s_microsteps_fail = (fail != 0); }

esp_err_t tmc2209_set_microsteps(uint16_t microsteps) {
    if (s_microsteps_fail) {
        return ESP_FAIL;
    }
    s_microsteps = microsteps;
    return ESP_OK;
}

/* Test-only: reset the stub's cached TMC state (resolution, chopper mode, and
 * fault-injection flags) to the post-init canonical defaults.  The host harness
 * reuses one process across many tests and must start each from a clean driver
 * state — a test that forces resolution 0 or a failed switch must not pollute
 * the next one. */
void host_tmc_reset(void) {
    s_microsteps = 64;
    s_chop_mode = TMC_CHOP_STEALTH;
    s_reinit_fail = false;
    s_microsteps_fail = false;
}

uint16_t tmc2209_get_microsteps(void) {
    return s_microsteps;
}

esp_err_t tmc2209_set_chop_mode(TmcChopMode mode) {
    s_chop_mode = mode;
    return ESP_OK;
}

TmcChopMode tmc2209_get_chop_mode(void) {
    return s_chop_mode;
}

esp_err_t tmc2209_reinit(uint16_t microsteps) {
    if (s_reinit_fail) {
        return ESP_FAIL;
    }
    s_microsteps = microsteps;
    s_chop_mode = TMC_CHOP_STEALTH;
    return ESP_OK;
}
