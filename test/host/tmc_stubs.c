/* tmc_stubs.c — host stub for the TMC2209 runtime microstep switch.
 *
 * The host regression compiles the REAL motors_microsteps.c and motors_power.c,
 * which reach into the TMC layer to change CHOPCONF.MRES, to query the active
 * resolution, and to re-initialise the drivers after a power loss.  The
 * single-wire UART driver is hardware-only, so this stub models the state the
 * real driver keeps: set_microsteps records the value, get_microsteps returns
 * it.  The tests exercise the selection/accounting logic, not the wire protocol
 * (covered by the proto acceptance suite on real hardware).
 */
#include "tmc.h"
#include "config.h"

static uint16_t s_microsteps = MOTORS_MICROSTEPS_TRACKING;

esp_err_t tmc2209_set_microsteps(uint16_t microsteps) {
    s_microsteps = microsteps;
    return ESP_OK;
}

uint16_t tmc2209_get_microsteps(void) {
    return s_microsteps;
}

esp_err_t tmc2209_reinit(void) {
    s_microsteps = MOTORS_MICROSTEPS_TRACKING;
    return ESP_OK;
}
