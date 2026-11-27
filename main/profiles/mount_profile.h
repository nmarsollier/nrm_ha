#pragma once

#include <stdint.h>
#include <stdbool.h>

/*
 * Mount profile — the build-time data that varies between NRM-HA (German
 * equatorial) and BFT (fork).  One provider of `mount_profile` is selected at
 * link time (see main/CMakeLists.txt); business code reads this struct and never
 * branches on mount names.
 *
 * The profile is the single source of configuration: mechanical scale, per-axis
 * mechanical limits/speeds, and the board electronics (GPIO, UART, coil current)
 * that differ between mounts.  GPIO and UART numbers are plain ints (not the
 * ESP-IDF enums) so this header stays free of driver dependencies and the host
 * regression can compile the profile providers standalone.
 */

typedef enum { AXIS_RA = 0, AXIS_DEC = 1 } AxisId;

typedef enum { GEOMETRY_GERMAN = 0, GEOMETRY_FORK = 1 } GeometryKind;

/* Per-axis mechanical data — only what genuinely differs per axis.  The scale
 * (full-step angle, reduction, microstep resolutions) and the speed ceilings
 * are shared by both axes of a mount and live on MountProfile. */
typedef struct {
    bool dir_positive_level;    /* DIR level that yields positive rotation */
    float hard_min_deg;         /* physical travel floor                  */
    float hard_max_deg;         /* physical travel ceiling                */
    float default_min_deg;      /* default operational limit              */
    float default_max_deg;      /* default operational limit              */
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

/* Per-axis electronics — STEP/DIR GPIO and the dedicated single-wire UART. */
typedef struct {
    int step_gpio;
    int dir_gpio;
    int uart_num;
    int uart_tx_gpio;
    int uart_rx_gpio;
} AxisHardwareProfile;

/* Board electronics shared by both axes of a mount.  The TMC2209 coil currents
 * (IRUN/IHOLD) are common to both drivers, not per axis. */
typedef struct {
    AxisHardwareProfile axes[2];
    int led_gpio;
    int buzzer_gpio;
    uint8_t irun;    /* TMC2209 IRUN field (0–31)  */
    uint8_t ihold;   /* TMC2209 IHOLD field (0–31) */
} HardwareProfile;

/* The mount profile — a single `const` instance provided at link time. */
typedef struct {
    const char *id;
    const char *name;
    uint32_t profile_revision;
    GeometryKind geometry;
    float ra_reference_offset_deg;

    /* Mechanical scale — shared by both axes (same motor, same reduction, same
     * TMC2209 resolution applied to both drivers). */
    float full_step_deg;           /* motor full-step angle (1.8 for NEMA 17) */
    float gear_ratio;              /* total reduction, motor shaft : axis      */
    uint16_t canonical_microsteps; /* finest (tracking) resolution             */
    uint16_t slew_microsteps;      /* coarse slew resolution                   */

    /* Speed ceilings — shared by both axes of a mount. */
    float max_slew_dps;            /* slew speed ceiling                       */
    float max_manual_dps;          /* MoveAxis speed ceiling                   */

    AxisProfile axes[2];
    SlewProfile slew;
    HardwareProfile hardware;
} MountProfile;

extern const MountProfile mount_profile;
