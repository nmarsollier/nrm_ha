/* Geometry — geometry_german.c — German equatorial mount geometry.
 *
 * Purpose: convert between physical axis coordinates and equatorial
 * coordinates for a German equatorial mount in the southern hemisphere.
 * Pure — the site longitude, the fractional instant and the mechanical limits
 * arrive in the GeometryContext; no global state or wall clock is read.
 *
 * =========================================================================
 * MECHANICAL MODEL
 * =========================================================================
 *
 * Home position (mechanical zero):
 *   - Counterweight down, scope parallel to RA axis.
 *   - Scope points at the South Celestial Pole (SCP).
 *   - ra_axis = 0, dec_axis = 0.
 *   - Reported celestial coordinates: RA = LST, DEC = -90.
 *
 * At mechanical home the DEC movement plane is NOT aligned with the
 * meridian — it is rotated ~90 from it.  If you move only DEC from
 * home, the scope sweeps laterally, not toward the zenith.  To make
 * DEC open toward the zenith, the RA axis must first rotate to
 * roughly +/-90 from home.
 *
 * The angle at which the DEC plane aligns with the meridian is a fixed
 * mechanical constant: HOME_RA_OFFSET_DEG = 90.
 *
 * =========================================================================
 * TWO MECHANICAL SOLUTIONS
 * =========================================================================
 *
 * A German equatorial can point at the same sky position from two
 * mechanically-distinct configurations (one on each side of the pier).
 * Both are computed, and the side is selected by a destination-side policy:
 *
 *   pierEast (0):  ra = HA - HOME_RA_OFFSET,   dec =  DEC + 90
 *   pierWest (1):  ra = HA + HOME_RA_OFFSET,   dec = -(DEC + 90)
 *
 *   target west of the meridian (HA >= 0)  ->  pierEast
 *   target east of the meridian (HA <  0)  ->  pierWest
 *
 * AUTO applies an asymmetric policy keyed on the current side (the sign of the
 * linear DEC axis):
 *
 *   From WEST -> flip to EAST as soon as the target crosses the meridian
 *                (HA >= 0); if EAST is unreachable, reject — never silently
 *                stay WEST.
 *   From EAST -> keep EAST while it is mechanically reachable, regardless of
 *                hour angle; return to WEST only when EAST is out of limits
 *                and WEST is reachable.  This keeps post-flip Slew-and-Center
 *                gotos from snapping back across the meridian.
 *
 * An explicit EAST/WEST request forces that side and rejects the goto when it
 * is unreachable — so a client can demand a meridian flip and tell it apart
 * from an ordinary goto.
 *
 * The mechanical jump at HA = 0 is the flip itself — inherent to a German
 * equatorial, not a discontinuity to be avoided.
 *
 * DEC axis is a linear coordinate (NOT circular).  It is NOT
 * normalised with normalize_degf() — that would incorrectly wrap
 * +180 to -180, inverting the pier side sign.
 */
#include "mount_geometry.h"

#include <math.h>
#include <stdbool.h>

#include "mount_profile.h"

#include "motion/slew_ramp.h"

/* Normalise a circular angle to [-180, +180).  +180 -> -180. */
static float normalize_degf(float d) {
    d = fmodf(d, 360.0f);
    if (d >= 180.0f)  d -= 360.0f;
    if (d < -180.0f)  d += 360.0f;
    return d;
}

/* Normalise hours to [0, 24).  fmodf keeps this O(1) and, unlike a `while`
 * loop, cannot spin forever on a non-finite input. */
static float normalize_hoursf(float h) {
    h = fmodf(h, 24.0f);
    if (h < 0.0f) h += 24.0f;
    return h;
}

/* Validate against the mechanical limits snapshot in the context. */
static bool axis_within_limits(const GeometryContext *ctx, float ra, float dec) {
    return (ra >= ctx->limits.ra_min && ra <= ctx->limits.ra_max)
        && (dec >= ctx->limits.dec_min && dec <= ctx->limits.dec_max);
}

/* --------------------------------------------------------------------------
 * Equatorial -> Axis  (forward conversion)
 *
 * Two mechanically-distinct candidates.  `required` selects the pier side:
 * AUTO applies the asymmetric meridian policy (see the module header);
 * EAST/WEST force that side and reject when it is unreachable.
 * -------------------------------------------------------------------------- */
bool geometry_equatorial_to_axis(const GeometryContext *ctx,
                                 EquatorialCoordinates eq,
                                 AxisCoordinates current,
                                 PierSideRequirement required,
                                 float speed_dps,
                                 AxisCoordinates *out) {
    /* The German geometry does not compensate for arrival here: the mount layer
     * re-solves at the arrival instant (with the side locked).  `speed_dps` is
     * therefore unused. */
    (void) speed_dps;

    if (!isfinite(eq.ra_hours) || !isfinite(eq.dec_deg)) {
        return false;
    }

    if (eq.dec_deg < -90.0f || eq.dec_deg > 90.0f) {
        return false;
    }

    /* `required` is an internal, reusable function's parameter: a caller that
     * passes anything outside the AUTO/EAST/WEST range would index c[] out of
     * bounds below. */
    if (required < PIER_SIDE_AUTO || required > PIER_SIDE_WEST) {
        return false;
    }

    eq.ra_hours = normalize_hoursf(eq.ra_hours);

    double lst = geometry_gmst_hours(ctx->unix_seconds) + (ctx->lon_deg / 15.0);

    float ha_h = fmodf((float)(lst - eq.ra_hours), 24.0f);
    if (ha_h > 12.0f)  ha_h -= 24.0f;
    if (ha_h < -12.0f) ha_h += 24.0f;

    float ha_deg = ha_h * 15.0f;
    float dec_base = eq.dec_deg + 90.0f;

    typedef struct { float ra; float dec; int pier; } Candidate;
    Candidate c[2];

    /* pierEast: scope on east side of pier, pointing west.
     *   ra = -(offset - HA) = HA - offset   (negated for physical motor direction) */
    c[0].ra   = normalize_degf(ha_deg - mount_profile.ra_reference_offset_deg);
    c[0].dec  = dec_base;
    c[0].pier = 0;

    /* pierWest: scope on west side of pier, pointing east.
     *   ra = -(-offset - HA) = HA + offset   (negated for physical motor direction) */
    c[1].ra   = normalize_degf(ha_deg + mount_profile.ra_reference_offset_deg);
    c[1].dec  = -dec_base;
    c[1].pier = 1;

    /* Asymmetric AUTO policy, keyed on the current side (sign of the linear DEC
     * axis — the same convention as axis_to_equatorial() and STATE's pier_side):
     *   WEST -> flip EAST as soon as HA >= 0, no fallback (a flip must reject
     *           rather than silently stay WEST).
     *   EAST -> keep EAST while reachable; fall back to WEST only when EAST is
     *           out of limits and WEST is reachable. */
    int current_side = (current.dec_axis_deg >= 0.0f) ? PIER_SIDE_EAST
                                                      : PIER_SIDE_WEST;

    int desired;
    bool allow_fallback;
    if (required == PIER_SIDE_AUTO) {
        if (current_side == PIER_SIDE_EAST) {
            desired = PIER_SIDE_EAST;    /* stick to EAST */
            allow_fallback = true;       /* -> WEST only when EAST is out of limits */
        } else if (ha_deg >= 0.0f) {
            desired = PIER_SIDE_EAST;    /* the flip: must not degrade to staying WEST */
            allow_fallback = false;
        } else {
            desired = PIER_SIDE_WEST;    /* stay WEST; EAST allowed if WEST unreachable */
            allow_fallback = true;
        }
    } else {
        desired = required;              /* PIER_SIDE_EAST or PIER_SIDE_WEST */
        allow_fallback = false;
    }

    int best = -1;
    if (axis_within_limits(ctx, c[desired].ra, c[desired].dec)) {
        best = desired;
    } else if (allow_fallback
               && axis_within_limits(ctx, c[1 - desired].ra, c[1 - desired].dec)) {
        best = 1 - desired;
    }

    if (best < 0) {
        return false;
    }

    out->ra_axis_deg  = c[best].ra;
    out->dec_axis_deg = c[best].dec;
    out->pier_side    = c[best].pier;

    return true;
}

/* --------------------------------------------------------------------------
 * Axis -> Equatorial  (reverse conversion)
 *
 * The pier side is reliably encoded in the sign of dec_axis because
 * DEC is linear (not normalised):
 *
 *   dec_axis >= 0  ->  pierEast  ->  HA = ra + offset,  DEC = dec - 90
 *   dec_axis <  0  ->  pierWest  ->  HA = ra - offset,  DEC = -dec - 90
 * -------------------------------------------------------------------------- */
EquatorialCoordinates geometry_axis_to_equatorial(const GeometryContext *ctx,
                                                  AxisCoordinates axis) {
    EquatorialCoordinates out;

    /* DEC is computed directly for every position.  At dec_axis == 0 (the
     * exact pole) DEC is -90 and RA is degenerate: the formula still returns
     * a valid, if arbitrary, RA — no threshold erases the nearby cone. */
    float ha_deg, dec_deg;
    if (axis.dec_axis_deg >= 0.0f) {
        /* pierEast: stored = -(offset - HA) = HA - offset  ->  HA = ra + offset */
        ha_deg  = normalize_degf(axis.ra_axis_deg + mount_profile.ra_reference_offset_deg);
        dec_deg = axis.dec_axis_deg - 90.0f;
    } else {
        /* pierWest: stored = -(-offset - HA) = HA + offset  ->  HA = ra - offset */
        ha_deg  = normalize_degf(axis.ra_axis_deg - mount_profile.ra_reference_offset_deg);
        dec_deg = -axis.dec_axis_deg - 90.0f;
    }

    float ha_h = ha_deg / 15.0f;

    double lst = geometry_gmst_hours(ctx->unix_seconds) + (ctx->lon_deg / 15.0);

    out.ra_hours = normalize_hoursf((float)(lst - ha_h));

    if (dec_deg < -90.0f) dec_deg = -90.0f;
    if (dec_deg >  90.0f) dec_deg =  90.0f;
    out.dec_deg = dec_deg;

    return out;
}

/* --------------------------------------------------------------------------
 * Plan a goto: solve at the request instant, lock the pier side, estimate the
 * two-axis ramp duration and re-solve at the arrival instant on that side.
 * -------------------------------------------------------------------------- */
bool geometry_plan_goto(const GeometryContext *ctx, EquatorialCoordinates eq,
                        AxisCoordinates current, PierSideRequirement required,
                        float speed_dps, GotoPlan *plan) {
    if (!plan) {
        return false;
    }

    AxisCoordinates axis_now;
    if (!geometry_equatorial_to_axis(ctx, eq, current, required, 0.0f, &axis_now)) {
        return false;
    }
    PierSideRequirement locked = (axis_now.pier_side == PIER_SIDE_EAST)
                                 ? PIER_SIDE_EAST : PIER_SIDE_WEST;

    /* Two-axis slew: the slower axis (larger ramp duration) wins. */
    float ra_dist = fabsf(axis_now.ra_axis_deg - current.ra_axis_deg);
    float dec_dist = fabsf(axis_now.dec_axis_deg - current.dec_axis_deg);
    float duration = fmaxf(slew_ramp_duration(&mount_profile.slew, ra_dist, speed_dps),
                           slew_ramp_duration(&mount_profile.slew, dec_dist, speed_dps));

    /* Re-solve at the arrival instant on the locked side: same site/limits,
     * only the instant advances. */
    GeometryContext arrival = *ctx;
    arrival.unix_seconds = ctx->unix_seconds + (double) duration;
    if (!geometry_equatorial_to_axis(&arrival, eq, current, locked, 0.0f,
                                     &plan->target)) {
        return false;
    }
    plan->pier_side = locked;
    plan->duration_s = duration;
    return true;
}
