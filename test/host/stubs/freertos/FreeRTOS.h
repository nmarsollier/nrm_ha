#pragma once
/* Minimal FreeRTOS stand-in for host-side tests.  The primitives are real
 * (pthread-based) so the production motion task runs in its own thread and the
 * queue/notification/semaphore semantics match ESP-IDF closely enough to
 * exercise the scheduler and motion loop faithfully. */
#include <stdint.h>
#include <stddef.h>
#include <pthread.h>

typedef int BaseType_t;
typedef uint32_t UBaseType_t;
typedef uint32_t TickType_t;

/* portMUX_TYPE — a real spinlock on the target; a pthread mutex on the host. */
typedef struct {
    pthread_mutex_t mtx;
} portMUX_TYPE;

#define portMUX_INITIALIZER_UNLOCKED { PTHREAD_MUTEX_INITIALIZER }
#define portENTER_CRITICAL(m) pthread_mutex_lock(&(m)->mtx)
#define portEXIT_CRITICAL(m)  pthread_mutex_unlock(&(m)->mtx)

#define pdTRUE  1
#define pdFALSE 0
#define pdPASS  1
#define pdFAIL  0
#define portMAX_DELAY ((TickType_t)0xffffffffu)

#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))

typedef enum {
    eNoAction = 0,
    eSetBits,
    eIncrement,
    eSetValueWithOverwrite,
    eSetValueWithoutOverwrite,
} eNotifyAction;
