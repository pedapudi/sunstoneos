#ifndef CPU_NPU_HOST_TASK
#define CPU_NPU_HOST_TASK
#include "FreeRTOS.h"
TaskHandle_t xTaskCreateStaticAffinitySet(void (*)(void *),const char *,unsigned,void *,unsigned,StackType_t *,StaticTask_t *,unsigned);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
#define taskSCHEDULER_NOT_STARTED 0
int xTaskGetSchedulerState(void);
void xTaskNotifyGive(TaskHandle_t);
void vTaskDelay(unsigned);
void vTaskSuspend(void *);
#endif
