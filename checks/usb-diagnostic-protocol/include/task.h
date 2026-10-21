/* Model task API: one task, ticks advance only through vTaskDelay. */
#ifndef MODEL_TASK_H
#define MODEL_TASK_H
#include "FreeRTOS.h"
TickType_t xTaskGetTickCount(void);
void vTaskDelay(TickType_t ticks);
void vTaskStartScheduler(void);
TaskHandle_t xTaskCreateStatic(TaskFunction_t entry, const char *name, uint32_t depth, void *argument,
                               UBaseType_t priority, StackType_t *stack, StaticTask_t *state);
#endif
