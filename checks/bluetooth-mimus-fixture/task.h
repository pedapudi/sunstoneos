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
#ifdef __cplusplus
}
#endif
#endif
