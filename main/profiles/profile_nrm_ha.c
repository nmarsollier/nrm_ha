/* Profile — profile_nrm_ha.c — NRM-HA (German equatorial) data.
 *
 * German equatorial mount, southern-hemisphere model, 3:1 belt × 100:1
 * harmonic drive (300:1 total), 90° RA reference offset.
 *
 * Electronics: ESP32-S3 44-pin, TMC2209 per axis on single-wire UART
 * (UART1 = RA, UART2 = DEC).  GPIO numbers are the raw ESP32-S3 pin numbers.
 * Pending physical verification: NRM uses GPIO 4 for the buzzer, BFT uses
 * GPIO 6 — kept as configured, not changed until confirmed on the real board.
 */
#include "mount_profile.h"

const MountProfile mount_profile = {
    .id = "nrm-ha",
    .name = "NRM-HA",
    .profile_revision = 1,
    .geometry = GEOMETRY_GERMAN,
    .ra_reference_offset_deg = 90.0f,

    .full_step_deg = 1.8f,
    .gear_ratio = 300.0f,
    .canonical_microsteps = 64,
    .slew_microsteps = 8,
    .max_slew_dps = 6.0f,
    .max_manual_dps = 1.0f,

    .axes = {
        [AXIS_RA] = {
            .dir_positive_level = true,   /* positive rotation -> DIR HIGH */
            .hard_min_deg = -180.0f,
            .hard_max_deg =  180.0f,
            .default_min_deg = -100.0f,
            .default_max_deg =  100.0f,
        },
        [AXIS_DEC] = {
            .dir_positive_level = true,
            .hard_min_deg = -180.0f,
            .hard_max_deg =  180.0f,
            .default_min_deg = -150.0f,
            .default_max_deg =  150.0f,
        },
    },

    .slew = {
        .min_speed_dps = 0.8f,
        .short_distance_deg = 2.0f,
        .gentle_distance_deg = 8.0f,
        .gentle_max_dps = 3.2f,
        .accel_distance_deg = 5.0f,
        .decel_distance_deg = 5.0f,
    },

    .hardware = {
        .axes = {
            [AXIS_RA] = {
                .step_gpio = 10,
                .dir_gpio = 9,
                .uart_num = 1,
                .uart_tx_gpio = 12,
                .uart_rx_gpio = 13,
            },
            [AXIS_DEC] = {
                .step_gpio = 15,
                .dir_gpio = 7,
                .uart_num = 2,
                .uart_tx_gpio = 17,
                .uart_rx_gpio = 18,
            },
        },
        .led_gpio = 5,
        .buzzer_gpio = 4,
        .irun = 14,
        .ihold = 10,
    },
};
