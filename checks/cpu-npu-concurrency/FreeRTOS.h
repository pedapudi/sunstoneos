#ifndef CPU_NPU_HOST_FREERTOS
#define CPU_NPU_HOST_FREERTOS
#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
typedef uintptr_t StackType_t;
typedef unsigned UBaseType_t;
typedef unsigned TickType_t;
typedef struct host_task { pthread_t thread; unsigned core; void (*entry)(void *);void *argument; } StaticTask_t;
typedef StaticTask_t *TaskHandle_t;
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
unsigned host_core(void);
#define portGET_CORE_ID() host_core()
#endif
