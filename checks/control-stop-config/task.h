#ifndef SS_CONTROL_STOP_TASK_H
#define SS_CONTROL_STOP_TASK_H
#include "FreeRTOS.h"
TaskHandle_t xTaskCreateStaticAffinitySet(void (*)(void *), const char *, uint32_t,
    void *, UBaseType_t, StackType_t *, StaticTask_t *, UBaseType_t);
uint32_t ulTaskNotifyTake(BaseType_t, TickType_t);
BaseType_t xTaskNotifyGive(TaskHandle_t);
void vTaskNotifyGiveFromISR(TaskHandle_t, BaseType_t *);
void vTaskDelay(TickType_t);
void vTaskSuspend(TaskHandle_t);
typedef enum { eRunning, eReady, eBlocked, eSuspended, eDeleted, eInvalid } eTaskState;
eTaskState eTaskGetState(TaskHandle_t);
#endif
