#ifndef DEADLINE_HOST_TASK_H
#define DEADLINE_HOST_TASK_H
#include "FreeRTOS.h"
typedef void *TaskHandle_t;
TaskHandle_t xTaskGetCurrentTaskHandle(void);
uint32_t ulTaskNotifyTake(BaseType_t,TickType_t);
void vTaskNotifyGiveFromISR(TaskHandle_t,BaseType_t *);
#endif
