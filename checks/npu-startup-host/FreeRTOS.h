/* Host model of the FreeRTOS declarations the NPU startup sequencer uses: one
 * EL1 task that runs while the scheduler runs (checks/npu-startup-host.c). */
#ifndef NPU_STARTUP_HOST_FREERTOS_H
#define NPU_STARTUP_HOST_FREERTOS_H
#include <stdint.h>
typedef void *TaskHandle_t;
typedef long BaseType_t;
typedef unsigned long UBaseType_t;
typedef uint32_t TickType_t;
typedef void *SemaphoreHandle_t;
typedef struct { void *unused; } StaticSemaphore_t;
#define configTICK_RATE_HZ 1000u
#define pdTRUE 1
#define pdFALSE 0
#define taskSCHEDULER_SUSPENDED 0
#define taskSCHEDULER_NOT_STARTED 1
#define taskSCHEDULER_RUNNING 2
BaseType_t xTaskGetSchedulerState(void);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
/* The native wake; the host model supplies its own and never calls this one. */
#define xTaskNotifyGive(task) ((void)(task))
#endif
