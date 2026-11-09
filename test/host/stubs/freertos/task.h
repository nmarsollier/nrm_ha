#pragma once
#include "freertos/FreeRTOS.h"

typedef void *TaskHandle_t;

BaseType_t xTaskNotify(TaskHandle_t task, uint32_t value, eNotifyAction action);
BaseType_t xTaskNotifyGive(TaskHandle_t task);
uint32_t ulTaskNotifyTake(BaseType_t clear_count, TickType_t wait_ticks);
void vTaskDelay(TickType_t ticks);
void vTaskDelete(TaskHandle_t task);
BaseType_t xTaskCreatePinnedToCore(void (*fn)(void *), const char *name,
                                   uint32_t stack, void *arg, uint32_t prio,
                                   TaskHandle_t *out, int core);
