/* Host — pthread-backed FreeRTOS primitives.  The production motion task runs
 * in a real thread; the queue/notification/semaphore semantics match ESP-IDF
 * closely enough to exercise the scheduler, the queue, and the notification
 * wake-up faithfully. */
#include <pthread.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/* --------------------------------------------------------------------------
 * Time helper — convert a millisecond wait into an absolute realtime deadline.
 * portMAX_DELAY is treated as "block forever".
 * -------------------------------------------------------------------------- */
static void ms_abstime(TickType_t wait_ticks, struct timespec *out) {
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    uint64_t ns = (uint64_t) now.tv_sec * 1000000000ULL + (uint64_t) now.tv_nsec
                  + (uint64_t) wait_ticks * 1000000ULL;
    out->tv_sec = (time_t) (ns / 1000000000ULL);
    out->tv_nsec = (long) (ns % 1000000000ULL);
}

/* --------------------------------------------------------------------------
 * Queue
 * -------------------------------------------------------------------------- */
struct host_queue {
    pthread_mutex_t mtx;
    pthread_cond_t cv;
    size_t item_size;
    size_t length;
    size_t head;     /* index of the oldest item */
    size_t count;
    unsigned char *data;
};

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size) {
    struct host_queue *q = calloc(1, sizeof(*q));
    if (q == NULL) return NULL;
    pthread_mutex_init(&q->mtx, NULL);
    pthread_cond_init(&q->cv, NULL);
    q->item_size = item_size;
    q->length = length;
    q->data = malloc((size_t) length * item_size);
    if (q->data == NULL) { free(q); return NULL; }
    return q;
}

BaseType_t xQueueSendToBack(QueueHandle_t q, const void *item, TickType_t wait) {
    pthread_mutex_lock(&q->mtx);
    while (q->count == q->length) {
        if (wait == 0) { pthread_mutex_unlock(&q->mtx); return pdFALSE; }
        if (wait == portMAX_DELAY) {
            pthread_cond_wait(&q->cv, &q->mtx);
        } else {
            struct timespec ts; ms_abstime(wait, &ts);
            if (pthread_cond_timedwait(&q->cv, &q->mtx, &ts) != 0) {
                pthread_mutex_unlock(&q->mtx); return pdFALSE;
            }
        }
    }
    size_t tail = (q->head + q->count) % q->length;
    memcpy(q->data + tail * q->item_size, item, q->item_size);
    q->count++;
    pthread_cond_broadcast(&q->cv);
    pthread_mutex_unlock(&q->mtx);
    return pdTRUE;
}

static BaseType_t queue_receive(QueueHandle_t q, void *item, TickType_t wait, bool peek) {
    pthread_mutex_lock(&q->mtx);
    while (q->count == 0) {
        if (wait == 0) { pthread_mutex_unlock(&q->mtx); return pdFALSE; }
        if (wait == portMAX_DELAY) {
            pthread_cond_wait(&q->cv, &q->mtx);
        } else {
            struct timespec ts; ms_abstime(wait, &ts);
            if (pthread_cond_timedwait(&q->cv, &q->mtx, &ts) != 0) {
                pthread_mutex_unlock(&q->mtx); return pdFALSE;
            }
        }
    }
    memcpy(item, q->data + q->head * q->item_size, q->item_size);
    if (!peek) {
        q->head = (q->head + 1) % q->length;
        q->count--;
        pthread_cond_broadcast(&q->cv);
    }
    pthread_mutex_unlock(&q->mtx);
    return pdTRUE;
}

BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t wait) {
    return queue_receive(q, item, wait, false);
}

BaseType_t xQueuePeek(QueueHandle_t q, void *item, TickType_t wait) {
    return queue_receive(q, item, wait, true);
}

BaseType_t xQueueReset(QueueHandle_t q) {
    pthread_mutex_lock(&q->mtx);
    q->head = 0;
    q->count = 0;
    pthread_cond_broadcast(&q->cv);
    pthread_mutex_unlock(&q->mtx);
    return pdPASS;
}

void vQueueDelete(QueueHandle_t q) {
    if (q == NULL) return;
    pthread_mutex_destroy(&q->mtx);
    pthread_cond_destroy(&q->cv);
    free(q->data);
    free(q);
}

/* --------------------------------------------------------------------------
 * Semaphore
 * -------------------------------------------------------------------------- */
struct host_semaphore {
    pthread_mutex_t mtx;
    pthread_cond_t cv;
    uint32_t count;
    uint32_t max;
};

static SemaphoreHandle_t sem_create(uint32_t max, uint32_t initial) {
    struct host_semaphore *s = calloc(1, sizeof(*s));
    if (s == NULL) return NULL;
    pthread_mutex_init(&s->mtx, NULL);
    pthread_cond_init(&s->cv, NULL);
    s->max = max;
    s->count = initial;
    return s;
}

SemaphoreHandle_t xSemaphoreCreateBinary(void) {
    return sem_create(1, 0);
}

SemaphoreHandle_t xSemaphoreCreateCounting(UBaseType_t max, UBaseType_t initial) {
    return sem_create(max, initial);
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t sem) {
    pthread_mutex_lock(&sem->mtx);
    if (sem->count >= sem->max) {
        pthread_mutex_unlock(&sem->mtx);
        return pdFAIL;
    }
    sem->count++;
    pthread_cond_broadcast(&sem->cv);
    pthread_mutex_unlock(&sem->mtx);
    return pdTRUE;
}

BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t sem, BaseType_t *woken) {
    if (woken != NULL) *woken = pdFALSE;
    return xSemaphoreGive(sem);
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t wait_ticks) {
    pthread_mutex_lock(&sem->mtx);
    while (sem->count == 0) {
        if (wait_ticks == 0) { pthread_mutex_unlock(&sem->mtx); return pdFALSE; }
        if (wait_ticks == portMAX_DELAY) {
            pthread_cond_wait(&sem->cv, &sem->mtx);
        } else {
            struct timespec ts; ms_abstime(wait_ticks, &ts);
            if (pthread_cond_timedwait(&sem->cv, &sem->mtx, &ts) != 0) {
                pthread_mutex_unlock(&sem->mtx); return pdFALSE;
            }
        }
    }
    sem->count--;
    pthread_mutex_unlock(&sem->mtx);
    return pdTRUE;
}

void vSemaphoreDelete(SemaphoreHandle_t sem) {
    if (sem == NULL) return;
    pthread_mutex_destroy(&sem->mtx);
    pthread_cond_destroy(&sem->cv);
    free(sem);
}

/* --------------------------------------------------------------------------
 * Task + notification
 * -------------------------------------------------------------------------- */
struct host_task {
    pthread_t thread;
    pthread_mutex_t n_mtx;
    pthread_cond_t n_cv;
    uint32_t notify_value;
};

static _Thread_local struct host_task *tls_task = NULL;

/* Trampoline: the production entry point is passed separately. */
struct host_task_boot {
    void (*fn)(void *);
    void *arg;
    struct host_task *task;
};

static void *task_boot(void *arg) {
    struct host_task_boot *b = arg;
    tls_task = b->task;
    b->fn(b->arg);
    free(b);
    return NULL;
}

BaseType_t xTaskCreatePinnedToCore(void (*fn)(void *), const char *name,
                                   uint32_t stack, void *arg, uint32_t prio,
                                   TaskHandle_t *out, int core) {
    (void) name; (void) stack; (void) prio; (void) core;
    struct host_task *t = calloc(1, sizeof(*t));
    if (t == NULL) return pdFAIL;
    pthread_mutex_init(&t->n_mtx, NULL);
    pthread_cond_init(&t->n_cv, NULL);
    t->notify_value = 0;

    struct host_task_boot *b = malloc(sizeof(*b));
    b->fn = fn; b->arg = arg; b->task = t;

    if (pthread_create(&t->thread, NULL, task_boot, b) != 0) {
        free(b); free(t);
        return pdFAIL;
    }
    if (out != NULL) *out = t;
    return pdPASS;
}

BaseType_t xTaskNotify(TaskHandle_t task, uint32_t value, eNotifyAction action) {
    struct host_task *t = task;
    pthread_mutex_lock(&t->n_mtx);
    switch (action) {
        case eIncrement:            t->notify_value++; break;
        case eSetBits:              t->notify_value |= value; break;
        case eSetValueWithOverwrite:    t->notify_value = value; break;
        case eSetValueWithoutOverwrite: if (t->notify_value == 0) t->notify_value = value; break;
        case eNoAction:             break;   /* no value change, but still wake */
        default:                    break;
    }
    pthread_cond_broadcast(&t->n_cv);
    pthread_mutex_unlock(&t->n_mtx);
    return pdPASS;
}

BaseType_t xTaskNotifyGive(TaskHandle_t task) {
    return xTaskNotify(task, 0, eIncrement);
}

uint32_t ulTaskNotifyTake(BaseType_t clear_count, TickType_t wait_ticks) {
    struct host_task *t = tls_task;
    if (t == NULL) return 0;
    pthread_mutex_lock(&t->n_mtx);
    while (t->notify_value == 0) {
        if (wait_ticks == 0) { pthread_mutex_unlock(&t->n_mtx); return 0; }
        if (wait_ticks == portMAX_DELAY) {
            pthread_cond_wait(&t->n_cv, &t->n_mtx);
        } else {
            struct timespec ts; ms_abstime(wait_ticks, &ts);
            if (pthread_cond_timedwait(&t->n_cv, &t->n_mtx, &ts) != 0) {
                pthread_mutex_unlock(&t->n_mtx); return 0;
            }
        }
    }
    uint32_t value = t->notify_value;
    t->notify_value = (clear_count == pdTRUE) ? 0 : value - 1;
    pthread_mutex_unlock(&t->n_mtx);
    return value;
}

void vTaskDelay(TickType_t ticks) {
    struct timespec ts = { .tv_sec = (time_t) (ticks / 1000),
                           .tv_nsec = (long) (ticks % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

void vTaskDelete(TaskHandle_t task) {
    (void) task;
    pthread_exit(NULL);
}
