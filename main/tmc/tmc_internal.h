/* tmc_internal.h — internal API shared among TMC module files
 *
 * Functions and variables exposed here are for use ONLY within the
 * main/tmc/ module. No file outside tmc/ should include this header.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "tmc.h"

#include "mount_profile.h"

/* ── Transport constants ───────────────────────────────────── */

#define TMC_BAUD_RATE    115200
#define TMC2209_ADDRESS  0x00   /* MS1=GND, MS2=GND */

/* ── TMC2209 register addresses ────────────────────────────── */

#define TMC_REG_GCONF      0x00
#define TMC_REG_IFCNT      0x02
#define TMC_REG_IOIN       0x06
#define TMC_REG_IHOLD_IRUN 0x10
#define TMC_REG_CHOPCONF   0x6C
#define TMC_REG_DRV_STATUS 0x6F
#define TMC_REG_PWMCONF    0x70

/* ── Per-axis selection ──────────────────────────────────────
 *
 * The drivers are addressed by axis index (0 = RA, 1 = DEC).  The UART and GPIO
 * wiring comes from the selected mount profile (mount_profile.hardware), the
 * single source of electronic configuration — no second per-axis table. */

#define TMC_AXIS_COUNT 2

/* Axis name for logging. */
static inline const char *tmc_axis_name(int axis) {
    return (axis == AXIS_RA) ? "RA" : "DEC";
}

/* Per-axis UART readiness — set by tmc2209_hw_init(). */
extern bool tmc_uart_ok[TMC_AXIS_COUNT];

/* ── UART access serialisation (tmc_uart.c) ────────────────── */

esp_err_t tmc_uart_lock(void);
void tmc_uart_unlock(void);

/* ── UART register primitives (tmc_uart.c) ───────────────────
 * Take the axis index; the pin/UART numbers are read from the profile. */

esp_err_t tmc_write_register(int axis, uint8_t reg, uint32_t value);
esp_err_t tmc_read_register(int axis, uint8_t reg, uint32_t *value);

/* ── MRES conversion (tmc_driver.c) ────────────────────────── */

bool tmc_microsteps_to_mres(uint16_t ms, uint8_t *mres);

/* Read the SPREAD pin level (IOIN[8]) from a driver (tmc_driver.c).  Returns
 * false when the read fails — the caller then assumes SPREAD=LOW. */
bool tmc_read_spread_pin(int axis, bool *spread_high);

/* ── Driver configuration — shared by hw_init and reinit (tmc_driver.c) ── */

esp_err_t tmc_configure_drivers(uint16_t microsteps);

/* Active microstep resolution — written by tmc_configure_drivers() and
 * tmc2209_set_microsteps(), read by tmc2209_get_microsteps() (tmc_driver.c). */
extern uint16_t tmc_microsteps;

/* Active chopper mode — written by tmc_configure_drivers() and
 * tmc2209_set_chop_mode(), read by tmc2209_get_chop_mode() (tmc_driver.c). */
extern TmcChopMode tmc_chop_mode;

/* ── Init state (tmc_get_initialized.c) ────────────────────── */

void tmc2209_set_initialized(bool initialized);
