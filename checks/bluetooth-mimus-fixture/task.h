#ifndef SUNSTONE_BT_MIMUS_FIXTURE_TASK_H
#define SUNSTONE_BT_MIMUS_FIXTURE_TASK_H
#include "FreeRTOS.h"
#ifdef __cplusplus
extern "C" {
#endif
TaskHandle_t xTaskGetCurrentTaskHandle(void);
BaseType_t xTaskGetSchedulerState(void);
TaskHandle_t xTaskCreateStatic(TaskFunction_t,const char *const,configSTACK_DEPTH_TYPE,void *const,UBaseType_t,StackType_t *const,StaticTask_t *const);
void vTaskDelay(TickType_t);
void vTaskSuspend(TaskHandle_t);
/* Task notifications on index 0; the fixture models them as functions. */
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
uint32_t ulTaskNotifyTake(BaseType_t clear,TickType_t ticks);
BaseType_t xTaskNotifyGive(TaskHandle_t);
#ifdef __cplusplus
}
#endif
#endif
