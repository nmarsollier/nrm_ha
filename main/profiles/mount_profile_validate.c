/* Profile — mount_profile_validate.c — validate the mount profile invariants.
 *
 * Shared by both profiles: reject a profile whose mechanical data could break
 * the clamp/step logic (non-finite ratio, invalid microsteps, unordered or
 * non-finite limits, non-positive speeds).
 */
#include "mount_profile.h"

#include <math.h>
#include <stddef.h>

static bool valid_microsteps(uint16_t m) {
    switch (m) {
        case 1: case 2: case 4: case 8: case 16:
        case 32: case 64: case 128: case 256:
            return true;
        default:
            return false;
    }
}

/* Scale and speed ceilings are shared by both axes, so they are validated once. */
static bool valid_common(const MountProfile *p) {
    if (!isfinite(p->full_step_deg) || p->full_step_deg <= 0.0f) return false;
    if (!isfinite(p->gear_ratio) || p->gear_ratio <= 0.0f) return false;
    if (!valid_microsteps(p->canonical_microsteps)) return false;
    if (!valid_microsteps(p->slew_microsteps)) return false;
    /* Canonical resolution must be a whole multiple of the coarse one. */
    if (p->canonical_microsteps % p->slew_microsteps != 0) return false;
    if (!isfinite(p->max_slew_dps) || p->max_slew_dps <= 0.0f) return false;
    if (!isfinite(p->max_manual_dps) || p->max_manual_dps <= 0.0f) return false;
    return true;
}

static bool valid_axis(const AxisProfile *a) {
    if (!isfinite(a->hard_min_deg) || !isfinite(a->hard_max_deg)) return false;
    if (a->hard_min_deg >= a->hard_max_deg) return false;
    /* Defaults must be finite and ordered inside the hard envelope (a NaN would
     * slip past every ordering comparison). */
    if (!isfinite(a->default_min_deg) || !isfinite(a->default_max_deg)) return false;
    if (a->default_min_deg < a->hard_min_deg || a->default_max_deg > a->hard_max_deg) return false;
    if (a->default_min_deg >= a->default_max_deg) return false;
    return true;
}

bool mount_profile_validate(const MountProfile *p) {
    if (p == NULL) return false;
    if (p->id == NULL || p->id[0] == '\0') return false;
    if (p->name == NULL || p->name[0] == '\0') return false;
    if (p->geometry != GEOMETRY_GERMAN && p->geometry != GEOMETRY_FORK) return false;
    if (!isfinite(p->ra_reference_offset_deg)) return false;

    if (!valid_common(p)) return false;

    if (!valid_axis(&p->axes[AXIS_RA])) return false;
    if (!valid_axis(&p->axes[AXIS_DEC])) return false;

    /* TMC2209 IRUN/IHOLD are 5-bit fields (IHOLD_IRUN [4:0] / [12:8]); a larger
     * value would bleed into adjacent bits and corrupt the register. */
    if (p->hardware.irun > 31 || p->hardware.ihold > 31) return false;

    const SlewProfile *s = &p->slew;
    if (!isfinite(s->min_speed_dps) || s->min_speed_dps <= 0.0f) return false;
    if (!isfinite(s->short_distance_deg) || s->short_distance_deg < 0.0f) return false;
    if (!isfinite(s->gentle_distance_deg) || s->gentle_distance_deg < 0.0f) return false;
    if (!isfinite(s->gentle_max_dps) || s->gentle_max_dps <= 0.0f) return false;
    if (!isfinite(s->accel_distance_deg) || s->accel_distance_deg < 0.0f) return false;
    if (!isfinite(s->decel_distance_deg) || s->decel_distance_deg < 0.0f) return false;

    return true;
}
