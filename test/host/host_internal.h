#pragma once
/* Shared state between the host mocks (gpio, rmt) and the test API. */
#include <stdint.h>
#include <stdbool.h>

/* DIR GPIO level for each axis, as last written by gpio_set_level.
 * Index 0 = RA (RA_DIR_GPIO), index 1 = DEC (DEC_DIR_GPIO). */
extern int host_dir_level[2];

/* Synchronized DIR accessors (host_gpio.c). */
int  host_dir_level_read(int axis);
void host_dir_level_set(int axis, int level);

/* Production DIR-write cache reset (motors_hw.c), used by host_dir_level_set to
 * keep the mock's physical level and the production cache consistent. */
void motors_hw_reset_dir_cache(void);

/* One observed STEP pulse. */
typedef struct {
    int axis;              /* 0 = RA, 1 = DEC */
    int64_t t_us;          /* esp_timer_get_time() at transmit */
    int dir_gpio;          /* DIR GPIO level at transmit time */
    uint32_t num_symbols;
    uint16_t dur0, dur1;   /* first symbol durations (ticks) */
    uint8_t lvl0, lvl1;    /* first symbol levels */
} host_step_t;

/* Recorded STEP transmissions (thread-safe). */
int host_step_count(void);
void host_step_get(int index, host_step_t *out);
void host_step_clear(void);

/* RMT fault injection knobs (settable from the Python tests):
 *   tx_error_axis       -1=none, 0=RA, 1=DEC: rmt_transmit returns ESP_FAIL
 *   absent_completion   -1=none, 0=RA, 1=DEC: rmt_transmit returns OK but never
 *                        signals TX-done, so the wait times out
 *   suppress_axis       -1=none, 0=RA, 1=DEC: rmt_transmit returns OK and
 *                        signals done, but records no STEP (motor physically
 *                        silent while the firmware still counts the step)
 */
extern int host_rmt_tx_error_axis;
extern int host_rmt_absent_completion;
extern int host_rmt_suppress_axis;

/* TMC stub reset (tmc_stubs.c): reset the cached resolution/chopper mode and
 * fault-injection flags to the post-init canonical defaults. */
void host_tmc_reset(void);

/* Controlled RMT mode (host_rmt.c): separate accept / emit / complete. */
int  host_rmt_set_controlled(int axis, int on);
void host_rmt_emit(int axis);
void host_rmt_emit_one(int axis);
void host_rmt_emit_partial(int axis, int n);
void host_rmt_complete(int axis);
int  host_rmt_pending_pulses(int axis);
int  host_rmt_pending_dir(int axis);
void host_rmt_reset_controlled(void);
int  host_rmt_transmit_raw(int axis, int num_pulses);
void host_rmt_set_tx_error_after(int axis, int n);
void host_rmt_arm_complete_barrier(int arm);
int  host_rmt_reached_complete_barrier(void);
void host_rmt_release_complete_barrier(void);
int  host_rmt_del_channel_entered(void);
int  host_rmt_freed_during_publication(void);
