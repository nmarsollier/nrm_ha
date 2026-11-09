#pragma once
#include <stdint.h>
/* Monotonic microsecond clock (host: CLOCK_MONOTONIC). */
int64_t esp_timer_get_time(void);
