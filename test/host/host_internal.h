#pragma once
/* Shared state between the host mocks (gpio, rmt) and the test API. */
#include <stdint.h>
#include <stdbool.h>

/* DIR GPIO level for each axis, as last written by gpio_set_level.
 * Index 0 = RA (RA_DIR_GPIO), index 1 = DEC (DEC_DIR_GPIO). */
extern int host_dir_level[2];

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
