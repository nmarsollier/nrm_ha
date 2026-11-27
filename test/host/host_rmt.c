/* Host — RMT mock.  Records every STEP pulse the motion task emits so tests can
 * observe the actual transmissions (per axis, with direction), and injects the
 * failure modes the production code must react to.
 *
 * The completion path mirrors ESP-IDF: on TX-done the mock invokes the
 * registered on_trans_done callback, which is the production on_ra_tx_done /
 * on_dec_tx_done and gives the semaphore motors_rmt_wait_* actually waits on.
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#include "driver/rmt_tx.h"
#include "esp_timer.h"
#include "host_internal.h"
#include "mount_profile.h"

int host_rmt_tx_error_axis = -1;
int host_rmt_absent_completion = -1;
int host_rmt_suppress_axis = -1;

/* "Fail after N successful submits" — the first `s_tx_error_after[axis]` accepts
 * succeed, then the next fails (and stays failing until reset).  -1 = disabled.
 * Lets a test abort deterministically after a known completed batch is in flight. */
static int s_tx_error_after[2] = {-1, -1};
static int s_submit_count[2] = {0, 0};

/* Controlled mode — separates accept / emit / complete for deterministic tests.
 * When s_controlled[axis] is set, rmt_transmit accepts a batch (returns OK and
 * remembers the committed pulses + expected direction) but neither records the
 * pulses nor fires TX-done.  The test then drives emission (host_rmt_emit) and
 * completion (host_rmt_complete) explicitly.  Auto mode (default) keeps the
 * synchronous accept==emit==complete behaviour.
 *
 * s_controlled / s_ctl are shared between the executor and the test thread and
 * are guarded by s_ctl_mtx.  The mutex is never held while firing TX-done
 * (on_done wakes the executor, which may re-enter rmt_transmit). */
static pthread_mutex_t s_ctl_mtx = PTHREAD_MUTEX_INITIALIZER;
static int s_controlled[2] = {0, 0};

/* Callback-publication lifetime guard.  host_rmt_complete() fires on_done
 * OUTSIDE s_ctl_mtx (the callback wakes the executor, which may re-enter
 * rmt_transmit), so a concurrent rmt_del_channel() — from the executor's
 * channel reset — could free the channel while a callback is still executing
 * on it: a use-after-free.  s_pub_inflight counts publications currently in
 * flight; it is armed before s_ctl_mtx is released and decremented after the
 * last on_done returns.  rmt_tx_wait_all_done() and rmt_del_channel() wait on
 * s_ctl_cv until it drains, giving the harness the lifetime guarantee the real
 * driver's rmt_tx_wait_all_done() provides.  Reuses s_ctl_mtx as the single
 * guard so there is no second lock whose order could invert against it. */
static pthread_cond_t s_ctl_cv = PTHREAD_COND_INITIALIZER;
static int s_pub_inflight = 0;
/* Lifetime observability: increments if a channel is freed while a callback
 * publication is still in flight (i.e. the lifetime guard was violated).  The
 * deterministic test asserts it stays 0; removing the wait in rmt_del_channel
 * makes it deterministically non-zero (the RED). */
static int s_freed_during_publication = 0;

/* Test barrier — pauses host_rmt_complete() between "prepare the completion"
 * (queue drained under s_ctl_mtx, lifetime guard armed) and "publish on_done",
 * so a test can request a concurrent channel reset/destruction in that window. */
static _Atomic int s_complete_barrier = 0;
static _Atomic int s_reached_complete_barrier = 0;

/* Test observable — set as soon as rmt_del_channel() is entered, so a test can
 * park the destructor inside it (post-fix it blocks there) before releasing the
 * publisher, making the concurrent free-vs-publish window deterministic. */
static _Atomic int s_del_channel_entered = 0;

#define HOST_CTL_DEPTH 4
#define HOST_CTL_MAX_SYMBOLS 256   /* one RMT buffer; reject anything larger */
static struct {
    struct host_rmt_channel *ch;
    int count;   /* number of committed-but-not-completed batches */
    struct {
        uint32_t num_symbols;
        uint32_t num_pulses;        /* total STEP pulses committed */
        uint32_t emitted_pulses;    /* progress: STEP pulses already emitted */
        int dir_expected;   /* DIR level at accept time (for incompatible-change detection) */
        rmt_symbol_word_t symbols[HOST_CTL_MAX_SYMBOLS];
    } q[HOST_CTL_DEPTH];
} s_ctl[2];

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
    if (gpio == mount_profile.hardware.axes[AXIS_RA].step_gpio) return 0;
    if (gpio == mount_profile.hardware.axes[AXIS_DEC].step_gpio) return 1;
    return -1;
}

static void record_step(int axis, int dir_gpio, rmt_symbol_word_t sym) {
    pthread_mutex_lock(&s_step_mtx);
    if (s_step_count < HOST_STEP_MAX) {
        host_step_t *s = &s_steps[s_step_count++];
        s->axis = axis;
        s->t_us = esp_timer_get_time();
        s->dir_gpio = dir_gpio;
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

    /* Fail after N successful accepts (deterministic abort injection). */
    if (axis >= 0 && axis < 2 && s_tx_error_after[axis] >= 0) {
        if (s_submit_count[axis] >= s_tx_error_after[axis]) {
            return ESP_FAIL;
        }
        s_submit_count[axis]++;
    }

    /* Controlled mode: accept (commit) the batch, defer emission + completion. */
    if (axis >= 0 && axis < 2) {
        pthread_mutex_lock(&s_ctl_mtx);
        bool controlled = s_controlled[axis] != 0;
        if (controlled) {
            if (s_ctl[axis].count >= HOST_CTL_DEPTH) {
                /* Queue full: the transmission is NOT accepted. */
                pthread_mutex_unlock(&s_ctl_mtx);
                return ESP_FAIL;
            }
            const rmt_symbol_word_t *syms = (const rmt_symbol_word_t *) payload;
            uint32_t num_symbols = (uint32_t) (payload_bytes / sizeof(rmt_symbol_word_t));
            if (num_symbols > HOST_CTL_MAX_SYMBOLS) {
                /* Cannot store the full payload — reject, never truncate. */
                pthread_mutex_unlock(&s_ctl_mtx);
                return ESP_FAIL;
            }
            uint32_t num_pulses = 0;
            for (uint32_t i = 0; i < num_symbols; i++) {
                if (syms[i].level0 == 1) num_pulses++;
            }
            int slot = s_ctl[axis].count;
            s_ctl[axis].ch = ch;
            s_ctl[axis].q[slot].num_symbols = num_symbols;
            s_ctl[axis].q[slot].num_pulses = num_pulses;
            s_ctl[axis].q[slot].dir_expected = host_dir_level[axis];
            s_ctl[axis].q[slot].emitted_pulses = 0;
            memcpy(s_ctl[axis].q[slot].symbols, syms,
                   num_symbols * sizeof(rmt_symbol_word_t));
            s_ctl[axis].count++;
            pthread_mutex_unlock(&s_ctl_mtx);
            return ESP_OK;
        }
        pthread_mutex_unlock(&s_ctl_mtx);
    }

    uint32_t num_symbols = 0;
    /* A suppressed axis is electrically silent: the firmware receives an OK
     * and a completion, but no STEP pulse reaches the motor.  Record nothing. */
    if (host_rmt_suppress_axis != axis) {
        const rmt_symbol_word_t *syms = (const rmt_symbol_word_t *) payload;
        num_symbols = (uint32_t) (payload_bytes / sizeof(rmt_symbol_word_t));
        for (uint32_t i = 0; i < num_symbols; i++) {
            if (syms[i].level0 == 1) {   /* a STEP pulse begins with level0 high */
                record_step(axis, host_dir_level[axis], syms[i]);
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
    if (ch == NULL) return ESP_OK;

    atomic_store(&s_del_channel_entered, 1);

    /* Never free a channel while a callback publication is still in flight:
     * the executor may reset a channel concurrently with host_rmt_complete()
     * firing on_done on it.  Block until the publication drains — the lifetime
     * the real driver guarantees via rmt_tx_wait_all_done().  This cannot
     * deadlock: the publication does not depend on the channel being freed. */
    pthread_mutex_lock(&s_ctl_mtx);
    while (s_pub_inflight > 0) {
        pthread_cond_wait(&s_ctl_cv, &s_ctl_mtx);
    }
    /* The wait above guarantees no publication is in flight at the free point;
     * a regression that skips the wait frees under a live publication and is
     * caught here deterministically. */
    if (s_pub_inflight > 0) {
        s_freed_during_publication++;
    }
    /* Discard the controlled queue's reference AND its pending batches: work
     * belonging to a destroyed channel must not survive to be re-associated with
     * a recreated channel.  Clearing count (not just the channel pointer)
     * prevents a later accept from appending behind stale batches and a later
     * complete()/emit() from resurrecting them.  Only the axis whose channel is
     * being destroyed is cleared — a queue pointing at a different, still-live
     * channel is untouched. */
    if (s_ctl[0].ch == ch) {
        s_ctl[0].ch = NULL;
        s_ctl[0].count = 0;
    }
    if (s_ctl[1].ch == ch) {
        s_ctl[1].ch = NULL;
        s_ctl[1].count = 0;
    }
    pthread_mutex_unlock(&s_ctl_mtx);

    free(ch);
    return ESP_OK;
}

esp_err_t rmt_del_encoder(rmt_encoder_handle_t enc) {
    free(enc);
    return ESP_OK;
}

esp_err_t rmt_tx_wait_all_done(rmt_channel_handle_t ch, int timeout_ms) {
    (void) ch;
    /* The real driver blocks until every pending TX-done event has been
     * dispatched; the harness models that as "no callback publication in
     * flight".  Honor the caller's timeout so a channel reset can proceed once
     * a concurrent host_rmt_complete() publication drains (or give up, in
     * which case rmt_del_channel still waits unboundedly before freeing). */
    pthread_mutex_lock(&s_ctl_mtx);
    if (s_pub_inflight == 0) {
        pthread_mutex_unlock(&s_ctl_mtx);
        return ESP_OK;
    }
    if (timeout_ms <= 0) {
        pthread_mutex_unlock(&s_ctl_mtx);
        return ESP_ERR_TIMEOUT;
    }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += (time_t) (timeout_ms / 1000);
    ts.tv_nsec += (long) (timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
    }
    int rc = 0;
    while (s_pub_inflight > 0 && rc == 0) {
        rc = pthread_cond_timedwait(&s_ctl_cv, &s_ctl_mtx, &ts);
    }
    pthread_mutex_unlock(&s_ctl_mtx);
    return (rc == ETIMEDOUT) ? ESP_ERR_TIMEOUT : ESP_OK;
}

/* ── Controlled-mode API (test-only) ─────────────────────────────────── */

/* Emit up to `max_count` further STEP pulses of batch `b` (from its progress
 * cursor), recording each with the physical DIR at emit time.  Returns the count
 * emitted and advances the progress cursor. */
static int emit_batch_pulses(int axis, int b, uint32_t max_count) {
    int dir = host_dir_level_read(axis);
    int emitted = 0;
    uint32_t pulse_idx = 0;
    for (uint32_t i = 0; i < s_ctl[axis].q[b].num_symbols && emitted < (int) max_count; i++) {
        if (s_ctl[axis].q[b].symbols[i].level0 != 1) continue;   /* idle symbol */
        if (pulse_idx >= s_ctl[axis].q[b].emitted_pulses) {
            record_step(axis, dir, s_ctl[axis].q[b].symbols[i]);
            emitted++;
        }
        pulse_idx++;
    }
    s_ctl[axis].q[b].emitted_pulses += (uint32_t) emitted;
    return emitted;
}

/* Enable/disable controlled mode for `axis`.  Refuses (returns -1) to disable
 * controlled mode while committed-but-uncompleted work is pending, so a mode
 * switch can never silently discard a transmission.  Enable always succeeds. */
int host_rmt_set_controlled(int axis, int on) {
    if (axis < 0 || axis > 1) return -1;
    pthread_mutex_lock(&s_ctl_mtx);
    if (!on && s_ctl[axis].count > 0) {
        pthread_mutex_unlock(&s_ctl_mtx);
        return -1;
    }
    s_controlled[axis] = on ? 1 : 0;
    pthread_mutex_unlock(&s_ctl_mtx);
    return 0;
}

/* Record the committed pulses of every not-yet-emitted batch (emission).  The
 * direction recorded is the PHYSICAL DIR level at emit time (host_dir_level), so
 * a premature DIR change in production is visible in the recorded pulses rather
 * than hidden by a frozen accept-time value.  The real symbol timing (period) is
 * replayed so a test can verify the applied frequency independently. */
void host_rmt_emit(int axis) {
    if (axis < 0 || axis > 1) return;
    pthread_mutex_lock(&s_ctl_mtx);
    for (int b = 0; b < s_ctl[axis].count; b++) {
        if (s_ctl[axis].q[b].emitted_pulses >= s_ctl[axis].q[b].num_pulses) continue;
        emit_batch_pulses(axis, b, s_ctl[axis].q[b].num_pulses - s_ctl[axis].q[b].emitted_pulses);
    }
    pthread_mutex_unlock(&s_ctl_mtx);
}

/* Fire TX-done (completion) for the leading FIFO prefix of fully-emitted
 * batches, and remove exactly those.  Un-emitted batches — and any emitted
 * batch behind an un-emitted one — are preserved in order.  If none is emitted,
 * the queue is untouched and no notification fires.  TX-done is fired OUTSIDE
 * the mutex: on_done gives the semaphore, waking the executor which may
 * re-enter rmt_transmit.  The channel's lifetime is pinned for the whole
 * publication (see s_pub_inflight), so a concurrent reset cannot free it under
 * a pending callback. */
void host_rmt_complete(int axis) {
    if (axis < 0 || axis > 1) return;
    struct host_rmt_channel *ch;
    int n = 0;
    int pulses[HOST_CTL_DEPTH];
    bool publish = false;
    pthread_mutex_lock(&s_ctl_mtx);
    ch = s_ctl[axis].ch;
    while (n < s_ctl[axis].count
           && s_ctl[axis].q[n].emitted_pulses >= s_ctl[axis].q[n].num_pulses
           && n < HOST_CTL_DEPTH) {
        pulses[n] = (int) s_ctl[axis].q[n].num_pulses;
        n++;
    }
    /* Shift the remaining (un-emitted, or behind an un-emitted) batches down,
     * preserving FIFO order. */
    int remaining = s_ctl[axis].count - n;
    if (remaining > 0 && n > 0) {
        memmove(&s_ctl[axis].q[0], &s_ctl[axis].q[n],
                (size_t) remaining * sizeof(s_ctl[axis].q[0]));
    }
    s_ctl[axis].count = remaining;

    /* Arm the lifetime guard BEFORE releasing s_ctl_mtx: the channel is pinned
     * from here until the callbacks below finish, so a concurrent
     * rmt_del_channel() cannot free it under a pending on_done. */
    if (n > 0 && ch != NULL && ch->on_done != NULL) {
        publish = true;
        s_pub_inflight++;
    }
    pthread_mutex_unlock(&s_ctl_mtx);

    if (publish) {
        /* Test barrier: deterministic window between preparation and
         * publication, for the concurrent reset/destruction race test. */
        if (atomic_load(&s_complete_barrier)) {
            atomic_store(&s_reached_complete_barrier, 1);
            while (atomic_load(&s_complete_barrier)) {
                struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000 };
                nanosleep(&ts, NULL);
            }
        }

        for (int i = 0; i < n; i++) {
            rmt_tx_done_event_data_t edata = { .num_symbols = pulses[i] };
            ch->on_done(ch, &edata, NULL);
        }

        pthread_mutex_lock(&s_ctl_mtx);
        s_pub_inflight--;
        if (s_pub_inflight == 0) {
            pthread_cond_broadcast(&s_ctl_cv);
        }
        pthread_mutex_unlock(&s_ctl_mtx);
    }
}

/* Emit only the FRONT un-emitted batch (FIFO), if any.  Lets a test build a
 * queue where the front is emitted and a later batch is still un-emitted, to
 * verify host_rmt_complete consumes only the emitted FIFO prefix. */
void host_rmt_emit_one(int axis) {
    if (axis < 0 || axis > 1) return;
    pthread_mutex_lock(&s_ctl_mtx);
    for (int b = 0; b < s_ctl[axis].count; b++) {
        if (s_ctl[axis].q[b].emitted_pulses >= s_ctl[axis].q[b].num_pulses) continue;
        emit_batch_pulses(axis, b, s_ctl[axis].q[b].num_pulses - s_ctl[axis].q[b].emitted_pulses);
        break;   /* only the front */
    }
    pthread_mutex_unlock(&s_ctl_mtx);
}

/* Emit the first `n` STEP pulses of the FRONT un-emitted batch, leaving it
 * un-emitted (so a normal completion is impossible).  Represents a partially
 * emitted transmission: the pulses are recorded for independent observation but
 * the batch never gains a completion. */
void host_rmt_emit_partial(int axis, int n) {
    if (axis < 0 || axis > 1 || n <= 0) return;
    pthread_mutex_lock(&s_ctl_mtx);
    for (int b = 0; b < s_ctl[axis].count; b++) {
        if (s_ctl[axis].q[b].emitted_pulses >= s_ctl[axis].q[b].num_pulses) continue;
        emit_batch_pulses(axis, b, (uint32_t) n);
        break;   /* only the front */
    }
    pthread_mutex_unlock(&s_ctl_mtx);
}

/* Number of pulses committed but not yet emitted for `axis`. */
int host_rmt_pending_pulses(int axis) {
    if (axis < 0 || axis > 1) return 0;
    pthread_mutex_lock(&s_ctl_mtx);
    int total = 0;
    for (int b = 0; b < s_ctl[axis].count; b++) {
        total += (int) (s_ctl[axis].q[b].num_pulses - s_ctl[axis].q[b].emitted_pulses);
    }
    pthread_mutex_unlock(&s_ctl_mtx);
    return total;
}

/* DIR level expected at accept time for the first pending batch, or -1 if none.
 * Lets a test detect a DIR change that happened while a transmission was pending. */
int host_rmt_pending_dir(int axis) {
    if (axis < 0 || axis > 1) return -1;
    pthread_mutex_lock(&s_ctl_mtx);
    int dir = (s_ctl[axis].count > 0) ? s_ctl[axis].q[0].dir_expected : -1;
    pthread_mutex_unlock(&s_ctl_mtx);
    return dir;
}

/* Force-clear the controlled state (mode off + empty queue), used by the harness
 * teardown/reset — not by a mode switch, which must not discard pending work. */
void host_rmt_reset_controlled(void) {
    pthread_mutex_lock(&s_ctl_mtx);
    s_controlled[0] = 0;
    s_controlled[1] = 0;
    s_ctl[0].count = 0;
    s_ctl[1].count = 0;
    s_ctl[0].ch = NULL;
    s_ctl[1].ch = NULL;
    pthread_mutex_unlock(&s_ctl_mtx);
    s_tx_error_after[0] = -1;
    s_tx_error_after[1] = -1;
    s_submit_count[0] = 0;
    s_submit_count[1] = 0;
    pthread_mutex_lock(&s_ctl_mtx);
    s_freed_during_publication = 0;
    pthread_mutex_unlock(&s_ctl_mtx);
    atomic_store(&s_complete_barrier, 0);
    atomic_store(&s_reached_complete_barrier, 0);
    atomic_store(&s_del_channel_entered, 0);
}

/* Test-only barrier around host_rmt_complete's callback publication. */
void host_rmt_arm_complete_barrier(int arm) {
    atomic_store(&s_complete_barrier, arm ? 1 : 0);
    if (!arm) atomic_store(&s_reached_complete_barrier, 0);
}
int host_rmt_reached_complete_barrier(void) {
    return atomic_load(&s_reached_complete_barrier);
}
void host_rmt_release_complete_barrier(void) {
    atomic_store(&s_complete_barrier, 0);
}
int host_rmt_del_channel_entered(void) {
    return atomic_load(&s_del_channel_entered);
}
int host_rmt_freed_during_publication(void) {
    pthread_mutex_lock(&s_ctl_mtx);
    int n = s_freed_during_publication;
    pthread_mutex_unlock(&s_ctl_mtx);
    return n;
}

/* Fail the (n+1)-th and later accepts for `axis` (n=-1 disables). */
void host_rmt_set_tx_error_after(int axis, int n) {
    if (axis < 0 || axis > 1) return;
    s_tx_error_after[axis] = n;
    s_submit_count[axis] = 0;
}

/* Directly drive rmt_transmit with a synthetic payload of `num_pulses` STEP
 * pulses, bypassing production.  Test-only: exercises the mock's queue-full and
 * truncation paths, which normal production never reaches (it commits at most
 * two ping/pong batches).  Returns 1 on accept, 0 on reject. */
int host_rmt_transmit_raw(int axis, int num_pulses) {
    if (axis < 0 || axis > 1 || num_pulses <= 0 || num_pulses > HOST_CTL_MAX_SYMBOLS) {
        return 0;
    }
    static struct host_rmt_channel fake[2] = {{0}, {0}};
    fake[axis].axis = axis;
    fake[axis].on_done = NULL;
    rmt_symbol_word_t syms[HOST_CTL_MAX_SYMBOLS];
    for (int i = 0; i < num_pulses; i++) {
        syms[i].level0 = 1; syms[i].duration0 = 4;
        syms[i].level1 = 0; syms[i].duration1 = 4;
    }
    esp_err_t e = rmt_transmit((rmt_channel_handle_t)&fake[axis], NULL, syms,
                               (size_t) num_pulses * sizeof(rmt_symbol_word_t), NULL);
    return (e == ESP_OK) ? 1 : 0;
}
