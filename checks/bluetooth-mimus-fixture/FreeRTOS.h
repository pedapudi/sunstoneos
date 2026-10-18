#ifndef SUNSTONE_BT_MIMUS_FIXTURE_FREERTOS_H
#define SUNSTONE_BT_MIMUS_FIXTURE_FREERTOS_H
#include <stdint.h>
#include <assert.h>
typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef uint32_t TickType_t;
typedef uintptr_t StackType_t;
typedef struct {uintptr_t words[32];} StaticTask_t;
typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);
#define configSTACK_DEPTH_TYPE uint32_t
#define configASSERT(x) assert(x)
#define taskSCHEDULER_NOT_STARTED 0
#define taskSCHEDULER_RUNNING 1
#ifdef __cplusplus
extern "C" {
#endif
void fixture_task_yield(void);
#ifdef __cplusplus
}
#endif
#define taskYIELD() fixture_task_yield()
#endif
