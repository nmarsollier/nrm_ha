#pragma once

#include "driver/gpio.h"
#include "driver/uart.h"

/*
 * Common board electronics — identical across NRM-HA and BFT.
 */

/* Step / direction pins — direct to the TMC2209 STEP/DIR inputs (3.3 V logic,
 * no level shifting). */
#define RA_STEP_GPIO   GPIO_NUM_10
#define RA_DIR_GPIO    GPIO_NUM_9
#define DEC_STEP_GPIO  GPIO_NUM_15
#define DEC_DIR_GPIO   GPIO_NUM_7

/* TMC2209 drivers — one single-wire UART per axis (TX through a 1 kΩ series
 * resistor, RX direct to the PDN_UART line). */
#define TMC_RA_UART_NUM   UART_NUM_1
#define TMC_RA_TX_GPIO    GPIO_NUM_12
#define TMC_RA_RX_GPIO    GPIO_NUM_13
#define TMC_DEC_UART_NUM  UART_NUM_2
#define TMC_DEC_TX_GPIO   GPIO_NUM_17
#define TMC_DEC_RX_GPIO   GPIO_NUM_18

/* Status LED — external indicator on GPIO 5, driven via LEDC PWM.  The GPIO
 * sources current (3.3 V) into the anode through a series resistor; the
 * cathode is tied direct to GND. */
#define LED_GPIO        GPIO_NUM_5

/* Active event buzzer — small module, I/O pin driven from the GPIO (HIGH = beep).
 * Pending physical verification: NRM used GPIO 6, BFT used GPIO 4. */
#define BUZZER_GPIO     GPIO_NUM_6

/* TMC2209 coil current (0–31 fields, IRUN/IHOLD).  Provisional values —
 * tune on the bench for the NEMA 17; see TMC.md. */
#define TMC_IRUN  14
#define TMC_IHOLD 10
