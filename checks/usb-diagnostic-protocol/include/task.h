/* Model task API: one task, ticks advance only through vTaskDelay. */
#ifndef MODEL_TASK_H
#define MODEL_TASK_H
#include "FreeRTOS.h"
TickType_t xTaskGetTickCount(void);
void vTaskDelay(TickType_t ticks);
void vTaskStartScheduler(void);
/* The model runs one task with no preemption, so a critical section has no
 * effect and the current task is the single model task. */
#define taskENTER_CRITICAL() ((void)0)
#define taskEXIT_CRITICAL() ((void)0)
TaskHandle_t xTaskGetCurrentTaskHandle(void);
TaskHandle_t xTaskCreateStatic(TaskFunction_t entry, const char *name, uint32_t depth, void *argument,
                               UBaseType_t priority, StackType_t *stack, StaticTask_t *state);
#endif
