/* Host — profile validator negative-case driver.
 *
 * Compiles mount_profile_validate.c + profile_nrm_ha.c and exposes a tiny API
 * so the Python test can confirm the validator accepts the real profile and
 * rejects mutated copies (NaN defaults, bad microsteps, unordered limits, …).
 */
#include "mount_profile.h"

#include <math.h>
#include <stdbool.h>

/* The real profile must validate. */
bool host_profile_valid(void) {
    return mount_profile_validate(&mount_profile);
}

typedef void (*mutate_fn)(MountProfile *);

/* True when the validator rejects the mutated copy. */
static bool rejected_when(mutate_fn mutate) {
    MountProfile p = mount_profile;
    mutate(&p);
    return !mount_profile_validate(&p);
}

static void nan_dec_default_min(MountProfile *p) { p->axes[AXIS_DEC].default_min_deg = NAN; }
static void nan_dec_default_max(MountProfile *p) { p->axes[AXIS_DEC].default_max_deg = NAN; }
static void nan_ra_default_min(MountProfile *p) { p->axes[AXIS_RA].default_min_deg = NAN; }
static void nan_ra_default_max(MountProfile *p) { p->axes[AXIS_RA].default_max_deg = NAN; }
static void bad_microsteps(MountProfile *p) { p->axes[AXIS_RA].canonical_microsteps = 3; }
static void unordered_hard_limits(MountProfile *p) { p->axes[AXIS_RA].hard_max_deg = p->axes[AXIS_RA].hard_min_deg - 1.0f; }
static void zero_gear(MountProfile *p) { p->axes[AXIS_RA].gear_ratio = 0.0f; }
static void default_outside_envelope(MountProfile *p) { p->axes[AXIS_RA].default_max_deg = p->axes[AXIS_RA].hard_max_deg + 1.0f; }
static void noncanonical_slew(MountProfile *p) { p->axes[AXIS_RA].canonical_microsteps = 16; p->axes[AXIS_RA].slew_microsteps = 3; }

bool host_reject_nan_dec_min(void) { return rejected_when(nan_dec_default_min); }
bool host_reject_nan_dec_max(void) { return rejected_when(nan_dec_default_max); }
bool host_reject_nan_ra_min(void) { return rejected_when(nan_ra_default_min); }
bool host_reject_nan_ra_max(void) { return rejected_when(nan_ra_default_max); }
bool host_reject_bad_microsteps(void) { return rejected_when(bad_microsteps); }
bool host_reject_unordered_limits(void) { return rejected_when(unordered_hard_limits); }
bool host_reject_zero_gear(void) { return rejected_when(zero_gear); }
bool host_reject_default_outside(void) { return rejected_when(default_outside_envelope); }
bool host_reject_noncanonical_slew(void) { return rejected_when(noncanonical_slew); }
