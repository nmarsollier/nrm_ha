/* Motors - motors_microsteps.c
 *
 * Purpose: select the TMC2209 microstep resolution and chopper mode per motion —
 * coarse (the profile's slew microsteps, SpreadCycle) for large ramped slews,
 * fine (the profile's canonical microsteps, StealthChop) for everything else.
 *
 * The chopper mode follows the resolution so the StealthChop→SpreadCycle switch
 * happens once, at standstill, before motion starts — never mid-ramp (which is
 * what an automatic TPWMTHRS threshold would cause).
 *
 * The STEP counters stay in canonical fine units; the motion loop converts to
 * coarse pulses through motors_steps_per_pulse().  The active resolution is
 * owned by the TMC layer (tmc2209_get_microsteps), which only ever records a
 * value after verifying it on the driver — so it is always the real one.
 */
#include "motors_internal.h"

#include "tmc.h"

#include <math.h>

#include "esp_log.h"

static const char *TAG = "MOTORS_MICROSTEPS";

uint16_t motors_microsteps_active(void) {
    return tmc2209_get_microsteps();
}

uint32_t motors_steps_per_pulse(void) {
    /* Precondition: the active resolution is verified (non-zero).  The caller
     * (motors_microsteps_prepare) refuses motion before this is ever reached. */
    return (uint32_t) mount_profile.canonical_microsteps
           / (uint32_t) tmc2209_get_microsteps();
}

/*
 * Apply `target` to both drivers.  A no-op when the drivers are already at the
 * target — the TMC layer's cached value is authoritative (it is only set after
 * a verified read-back), so this never skips a needed switch.  On a failed
 * switch the mount is stopped and put into ERROR.
 */
static bool apply_microsteps(uint16_t target) {
    if (target == tmc2209_get_microsteps()) {
        return true;
    }

    if (tmc2209_set_microsteps(target) != ESP_OK) {
        ESP_LOGE(TAG, "microstep switch to %u failed — stop + ERROR", (unsigned) target);
        /* Abort the pending motion and enter ERROR; motors_enter_error_state()
         * closes admission and invalidates queued work. */
        motors_motion_stop();
        motors_enter_error_state();
        return false;
    }

    return true;
}

/*
 * Apply `mode` to both drivers.  Same contract as apply_microsteps(): a no-op
 * when the drivers are already in the mode, and stop+ERROR on a failed switch.
 */
static bool apply_chop_mode(TmcChopMode mode) {
    if (mode == tmc2209_get_chop_mode()) {
        return true;
    }

    if (tmc2209_set_chop_mode(mode) != ESP_OK) {
        ESP_LOGE(TAG, "chop mode switch to %s failed — stop + ERROR",
                 mode == TMC_CHOP_SPREAD ? "SpreadCycle" : "StealthChop");
        /* Abort the pending motion and enter ERROR; motors_enter_error_state()
         * closes admission and invalidates queued work. */
        motors_motion_stop();
        motors_enter_error_state();
        return false;
    }

    return true;
}

bool motors_microsteps_prepare(MotionCommandType type, int64_t ra_dist, int64_t dec_dist) {
    /* A driver that never confirmed a resolution (tmc2209_hw_init/reinit failed)
     * cannot emit steps: refuse motion rather than assume steps-per-pulse. */
    if (tmc2209_get_microsteps() == 0) {
        ESP_LOGE(TAG, "TMC resolution unverified — refusing motion");
        motors_enter_error_state();
        return false;
    }

    uint16_t target = mount_profile.canonical_microsteps;
    TmcChopMode chop = TMC_CHOP_STEALTH;

    if (type == MOTION_CMD_SLEW) {
        float ra_deg = motors_steps_to_deg(ra_dist);
        float dec_deg = motors_steps_to_deg(dec_dist);
        float max_deg = fmaxf(fabsf(ra_deg), fabsf(dec_deg));
        if (max_deg > mount_profile.slew.gentle_distance_deg) {
            target = mount_profile.slew_microsteps;
            chop = TMC_CHOP_SPREAD;
        }
    }

    return apply_microsteps(target) && apply_chop_mode(chop);
}
