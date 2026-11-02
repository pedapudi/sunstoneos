/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SHIM_TASK_H
#define SHIM_TASK_H
#include "FreeRTOS.h"
#define portMAX_DELAY 0xffffffffu
TaskHandle_t xTaskCreateStaticAffinitySet(void (*function)(void *), const char *name, uint32_t depth,
    void *argument, unsigned priority, StackType_t *stack, StaticTask_t *state, uint32_t affinity);
uint32_t ulTaskNotifyTake(int clear, TickType_t ticks);
int xTaskNotifyGive(TaskHandle_t task);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
void vTaskDelay(TickType_t ticks);
#endif
