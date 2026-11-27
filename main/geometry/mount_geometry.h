#pragma once

#include <stdbool.h>

/* Sidereal tracking rate (degrees/second). */
#define GEOMETRY_SIDEREAL_DPS 0.004178074f

/*
 * Pure mount geometry — coordinate spaces and conversion functions.
 *
 * The geometry is a pure layer: every conversion takes an explicit context
 * (site, instant, mechanical limits) and never reads global state, the wall
 * clock, NVS, GPIO or motor state.  The same inputs always produce the same
 * output.  The mount layer (mount_coordinate_tools.c) adapts these pure
 * functions to the current mutable state.
 *
 * Coordinate spaces:
 *   Equatorial (sky):   RA in decimal hours [0,24), DEC in degrees [-90,90].
 *   Axis (mechanical):  RA/DEC axis angles in degrees from HOME.  DEC is linear
 *                       and is NOT normalised as a circle.
 */

/* Equatorial (sky) coordinates. */
typedef struct {
    float ra_hours;
    float dec_deg;
} EquatorialCoordinates;

/* Mechanical axis coordinates.  `pier_side` is filled by the forward conversion. */
typedef struct {
    float ra_axis_deg;
    float dec_axis_deg;
    int pier_side;      /* 0 = pierEast, 1 = pierWest */
} AxisCoordinates;

/* Destination pier side requested for a goto (German equatorial policy). */
typedef enum {
    PIER_SIDE_AUTO = -1,
    PIER_SIDE_EAST = 0,
    PIER_SIDE_WEST = 1,
} PierSideRequirement;

/* Mechanical limits snapshot (immutable for a single conversion). */
typedef struct {
    float ra_min;
    float ra_max;
    float dec_min;
    float dec_max;
} GeometryLimits;

/* Immutable geometry context: site, fractional instant and limits snapshot. */
typedef struct {
    float lat_deg;       /* unused by the German model; the fork (PR3) uses it */
    float lon_deg;
    double unix_seconds; /* fractional unix instant */
    GeometryLimits limits;
} GeometryContext;

/* GMST in hours [0,24) for a fractional unix instant (USNO formula). */
double geometry_gmst_hours(double unix_seconds);

/* Local sidereal time in hours [0,24) for a longitude at a fractional instant. */
float geometry_lst(float lon_deg, double unix_seconds);

/* Reverse conversion — axis (mechanical) to equatorial (sky).  Pure. */
EquatorialCoordinates geometry_axis_to_equatorial(const GeometryContext *ctx,
                                                  AxisCoordinates axis);

/* Forward conversion — equatorial to axis, selecting the mechanical solution.
 * Pure.  `speed_dps` drives the arrival compensation: the German geometry
 * ignores it (the mount layer re-solves at the arrival instant); the fork
 * geometry advances each candidate by the sidereal rate over its travel time.
 * `required` is the German pier-side policy; the fork rejects non-AUTO. */
bool geometry_equatorial_to_axis(const GeometryContext *ctx,
                                 EquatorialCoordinates eq,
                                 AxisCoordinates current,
                                 PierSideRequirement required,
                                 float speed_dps,
                                 AxisCoordinates *out);

/* A planned goto: the final axis target, the selected pier side and the
 * estimated single-slew duration. */
typedef struct {
    AxisCoordinates target;
    int pier_side;      /* PIER_SIDE_EAST/WEST (german) or -1 (fork) */
    float duration_s;
} GotoPlan;

/* Plan a goto: select the mechanical solution and apply the arrival
 * compensation, returning the final axis target for the sky target.  The
 * geometry owns the strategy (German: lock side, re-solve at arrival; fork:
 * minimum travel with per-candidate advance) from a single context.  Pure. */
bool geometry_plan_goto(const GeometryContext *ctx, EquatorialCoordinates eq,
                        AxisCoordinates current, PierSideRequirement required,
                        float speed_dps, GotoPlan *plan);
