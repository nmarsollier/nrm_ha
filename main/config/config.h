#pragma once

#include "driver/gpio.h"
#include "hal/adc_types.h"

/*
 * Mount hardware configuration.
 *
 * Single source of truth for the build-time constants that vary between
 * mounts: pin assignments, motor resolution and mechanical reduction.
 * To adapt the firmware to a different mount, edit this file only —
 * no other module may hardcode these values.
 */

/* =========================================================================
 * Mount identity.
 * ========================================================================= */

/* Mount model identifier — reported in the serial protocol `capabilities`. */
#define MOUNT_NAME "NRM-HA"

/* =========================================================================
 * Pin assignments — NRM-HA (ESP32-S3 44-pin board).
 * ========================================================================= */

/* Step / direction pins (contiguous GPIO 14→9 for clean PCB routing):
 *   GPIO 14: STEP- RA
 *   GPIO 10: DIR- RA
 *   GPIO 12: STEP- DEC
 *   GPIO 9:  DIR- DEC
 */
#define RA_STEP_GPIO   GPIO_NUM_14
#define RA_DIR_GPIO    GPIO_NUM_10
#define DEC_STEP_GPIO  GPIO_NUM_12
#define DEC_DIR_GPIO   GPIO_NUM_9

/* Status LED — direct GPIO, LEDC PWM. */
#define LED_GPIO        GPIO_NUM_42

/* Passive buzzer — direct GPIO, LEDC PWM (2 kHz). */
#define BUZZER_GPIO     GPIO_NUM_41

/* External 5v5 rail detection — GPIO 1 (ADC1_CH0), 10k/10k divider on the
 * LM2596 5.5V output. */
#define POWER_SENSE_ADC_UNIT    ADC_UNIT_1
#define POWER_SENSE_ADC_CHANNEL ADC_CHANNEL_0

/* =========================================================================
 * Motor resolution & mechanics.
 * ========================================================================= */

/* Microstep resolution — closed-loop driver DIP-switch setting.
 * NRM-HA: 64 microsteps = 12800 steps/revolution. */
#define MOTORS_MICROSTEPS 64

/* Total gear reduction: 3:1 belt (20T→60T GT2 9mm) × 100:1 harmonic drive.
 * motor shaft turns : axis turns. */
#define TOTAL_GEAR_REDUCTION (300.0f)
