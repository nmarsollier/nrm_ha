/* Host — RMT mock.  Records every STEP pulse the motion task emits so tests can
 * observe the actual transmissions (per axis, with direction), and injects the
 * failure modes the production code must react to.
 *
 * The completion path mirrors ESP-IDF: on TX-done the mock invokes the
 * registered on_trans_done callback, which is the production on_ra_tx_done /
 * on_dec_tx_done and gives the semaphore motors_rmt_wait_* actually waits on.
 */
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "driver/rmt_tx.h"
#include "esp_timer.h"
#include "host_internal.h"
#include "config.h"

int host_rmt_tx_error_axis = -1;
int host_rmt_absent_completion = -1;
int host_rmt_suppress_axis = -1;

struct host_rmt_encoder {
    int dummy;
};

struct host_rmt_channel {
    int gpio;
    int axis;               /* 0 = RA, 1 = DEC */
    bool enabled;
    bool with_dma;
    rmt_tx_done_callback_t on_done;
};

#define HOST_STEP_MAX 200000
static host_step_t s_steps[HOST_STEP_MAX];
static int s_step_count = 0;
static pthread_mutex_t s_step_mtx = PTHREAD_MUTEX_INITIALIZER;

int host_step_count(void) {
    pthread_mutex_lock(&s_step_mtx);
    int n = s_step_count;
    pthread_mutex_unlock(&s_step_mtx);
    return n;
}

void host_step_get(int index, host_step_t *out) {
    pthread_mutex_lock(&s_step_mtx);
    if (out != NULL && index >= 0 && index < s_step_count) {
        *out = s_steps[index];
    } else if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    pthread_mutex_unlock(&s_step_mtx);
}

void host_step_clear(void) {
    pthread_mutex_lock(&s_step_mtx);
    s_step_count = 0;
    pthread_mutex_unlock(&s_step_mtx);
}

static int axis_for_gpio(int gpio) {
    if (gpio == RA_STEP_GPIO) return 0;
    if (gpio == DEC_STEP_GPIO) return 1;
    return -1;
}

static void record_step(int axis, rmt_symbol_word_t sym) {
    pthread_mutex_lock(&s_step_mtx);
    if (s_step_count < HOST_STEP_MAX) {
        host_step_t *s = &s_steps[s_step_count++];
        s->axis = axis;
        s->t_us = esp_timer_get_time();
        s->dir_gpio = host_dir_level[axis];
        s->num_symbols = 1;
        s->dur0 = sym.duration0;
        s->dur1 = sym.duration1;
        s->lvl0 = sym.level0;
        s->lvl1 = sym.level1;
    }
    pthread_mutex_unlock(&s_step_mtx);
}

esp_err_t rmt_new_tx_channel(const rmt_tx_channel_config_t *cfg, rmt_channel_handle_t *out) {
    struct host_rmt_channel *ch = calloc(1, sizeof(*ch));
    if (ch == NULL) return ESP_ERR_NO_MEM;
    ch->gpio = cfg->gpio_num;
    ch->axis = axis_for_gpio(cfg->gpio_num);
    ch->with_dma = cfg->flags.with_dma;
    *out = ch;
    return ESP_OK;
}

esp_err_t rmt_new_copy_encoder(const rmt_copy_encoder_config_t *cfg, rmt_encoder_handle_t *out) {
    (void) cfg;
    struct host_rmt_encoder *e = calloc(1, sizeof(*e));
    if (e == NULL) return ESP_ERR_NO_MEM;
    *out = e;
    return ESP_OK;
}

esp_err_t rmt_tx_register_event_callbacks(rmt_channel_handle_t ch,
                                          const rmt_tx_event_callbacks_t *cbs,
                                          void *user) {
    (void) user;
    ch->on_done = cbs->on_trans_done;
    return ESP_OK;
}

esp_err_t rmt_enable(rmt_channel_handle_t ch) {
    ch->enabled = true;
    return ESP_OK;
}

esp_err_t rmt_disable(rmt_channel_handle_t ch) {
    ch->enabled = false;
    return ESP_OK;
}

esp_err_t rmt_transmit(rmt_channel_handle_t ch, rmt_encoder_handle_t enc,
                       const void *payload, size_t payload_bytes,
                       const rmt_transmit_config_t *cfg) {
    (void) enc; (void) cfg;
    int axis = ch->axis;

    if (host_rmt_tx_error_axis == axis) {
        return ESP_FAIL;
    }

    uint32_t num_symbols = 0;
    /* A suppressed axis is electrically silent: the firmware receives an OK
     * and a completion, but no STEP pulse reaches the motor.  Record nothing. */
    if (host_rmt_suppress_axis != axis) {
        const rmt_symbol_word_t *syms = (const rmt_symbol_word_t *) payload;
        num_symbols = (uint32_t) (payload_bytes / sizeof(rmt_symbol_word_t));
        for (uint32_t i = 0; i < num_symbols; i++) {
            if (syms[i].level0 == 1) {   /* a STEP pulse begins with level0 high */
                record_step(axis, syms[i]);
            }
        }
    }

    /* Simulate TX-done by invoking the registered callback, which gives the
     * done semaphore the production wait takes.  "Absent completion" skips it. */
    if (host_rmt_absent_completion != axis && ch->on_done != NULL) {
        rmt_tx_done_event_data_t edata = { .num_symbols = (int) num_symbols };
        ch->on_done(ch, &edata, NULL);
    }
    return ESP_OK;
}

esp_err_t rmt_del_channel(rmt_channel_handle_t ch) {
    free(ch);
    return ESP_OK;
}

esp_err_t rmt_del_encoder(rmt_encoder_handle_t enc) {
    free(enc);
    return ESP_OK;
}

esp_err_t rmt_tx_wait_all_done(rmt_channel_handle_t ch, int timeout_ms) {
    (void) ch; (void) timeout_ms;
    return ESP_OK;
}
