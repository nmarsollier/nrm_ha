#pragma once

#include <stdint.h>
#include <stdbool.h>

/*
 * Mount mechanical profile — the build-time data that varies between NRM-HA
 * (German equatorial) and BFT (fork).  One provider of `mount_profile` is
 * selected at link time (see main/CMakeLists.txt); the business code reads
 * this struct and never branches on mount names.
 */

typedef enum { AXIS_RA = 0, AXIS_DEC = 1 } AxisId;

typedef enum { GEOMETRY_GERMAN = 0, GEOMETRY_FORK = 1 } GeometryKind;

/* Per-axis mechanical data. */
typedef struct {
    float full_step_deg;          /* motor full-step angle (1.8 for NEMA 17)  */
    float gear_ratio;             /* total reduction, motor shaft : axis     */
    uint16_t canonical_microsteps; /* finest (tracking) resolution            */
    uint16_t slew_microsteps;      /* coarse slew resolution                  */
    bool dir_positive_level;       /* DIR level that yields positive rotation */
    float hard_min_deg;            /* physical travel floor                  */
    float hard_max_deg;            /* physical travel ceiling                */
    float default_min_deg;         /* default operational limit              */
    float default_max_deg;         /* default operational limit              */
    float max_slew_dps;            /* slew speed ceiling                     */
    float max_manual_dps;          /* MoveAxis speed ceiling                 */
} AxisProfile;

/* Distance-based trapezoidal ramp profile. */
typedef struct {
    float min_speed_dps;
    float short_distance_deg;
    float gentle_distance_deg;
    float gentle_max_dps;
    float accel_distance_deg;
    float decel_distance_deg;
} SlewProfile;

/* The mount profile — a single `const` instance provided at link time. */
typedef struct {
    const char *id;
    const char *name;
    uint32_t profile_revision;
    GeometryKind geometry;
    /* False while the profile's geometry, limits and speeds are not yet wired
     * into the executor — motion is refused until the integration completes. */
    bool motion_ready;
    float ra_reference_offset_deg;
    AxisProfile axes[2];
    SlewProfile slew;
} MountProfile;

extern const MountProfile mount_profile;

/* Validate the profile invariants (finite/ordered/within-envelope).  Called at
 * boot; a false return is a configuration error. */
bool mount_profile_validate(const MountProfile *p);
