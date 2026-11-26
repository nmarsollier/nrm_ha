/* Profile — profile_bft.c — BFT (fork equatorial) mechanical data.
 *
 * Fork equatorial mount, both hemispheres, 5:1 belt × 100:1 harmonic drive
 * (500:1 total), no RA reference offset.
 */
#include "mount_profile.h"

const MountProfile mount_profile = {
    .id = "bft",
    .name = "BFT",
    .profile_revision = 1,
    .geometry = GEOMETRY_FORK,
    .ra_reference_offset_deg = 0.0f,

    .axes = {
        [AXIS_RA] = {
            .full_step_deg = 1.8f,
            .gear_ratio = 500.0f,
            .canonical_microsteps = 64,
            .slew_microsteps = 8,
            .dir_positive_level = false,  /* positive rotation -> DIR LOW */
            .hard_min_deg = -100.0f,
            .hard_max_deg =  100.0f,
            .default_min_deg = -100.0f,
            .default_max_deg =  100.0f,
            .max_slew_dps = 4.0f,
            .max_manual_dps = 1.0f,
        },
        [AXIS_DEC] = {
            .full_step_deg = 1.8f,
            .gear_ratio = 500.0f,
            .canonical_microsteps = 64,
            .slew_microsteps = 8,
            .dir_positive_level = false,
            .hard_min_deg = -135.0f,
            .hard_max_deg =   50.0f,
            .default_min_deg = -135.0f,
            .default_max_deg =   50.0f,
            .max_slew_dps = 4.0f,
            .max_manual_dps = 1.0f,
        },
    },

    .slew = {
        .min_speed_dps = 0.8f,
        .short_distance_deg = 2.0f,
        .gentle_distance_deg = 8.0f,
        .gentle_max_dps = 3.2f,
        .accel_distance_deg = 10.0f,
        .decel_distance_deg = 10.0f,
    },
};
