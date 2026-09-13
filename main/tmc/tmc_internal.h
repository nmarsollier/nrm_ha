/* tmc_internal.h — internal API shared among TMC module files
 *
 * Functions and variables exposed here are for use ONLY within the
 * main/tmc/ module. No file outside tmc/ should include this header.
 */

#pragma once

#include "tmc.h"
#include <stdint.h>

/*
 * Update the cached active microstep count.
 * Called by tmc_set_microsteps() after verifying the hardware latch.
 */
void tmc2209_set_active_microsteps(uint16_t microsteps);

/*
 * Per-axis init diagnostics — written by tmc_init.c as each driver is
 * configured.  Read by tmc_get_axis_status.c for the public getters.
 */
extern TmcAxisStatus tmc_axis_status[TMC_AXIS_COUNT];
extern TmcAxisError  tmc_axis_error[TMC_AXIS_COUNT];
