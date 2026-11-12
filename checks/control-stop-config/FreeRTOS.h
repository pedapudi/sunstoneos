#ifndef SS_CONTROL_STOP_FREERTOS_H
#define SS_CONTROL_STOP_FREERTOS_H
#include <assert.h>
#include <stdint.h>
typedef int32_t BaseType_t;
typedef uint64_t UBaseType_t;
typedef uint32_t TickType_t;
typedef uint64_t StackType_t;
typedef struct { uint64_t opaque[8]; } StaticTask_t;
struct control_test_task;
typedef struct control_test_task *TaskHandle_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
#define configASSERT(expression) assert(expression)
#ifndef INCLUDE_eTaskGetState
#define INCLUDE_eTaskGetState 1
#endif
unsigned ss_control_test_core(void);
void ss_control_test_sgi(unsigned);
UBaseType_t ss_control_test_mask(void);
void ss_control_test_unmask(UBaseType_t);
#define portGET_CORE_ID() ss_control_test_core()
#define portYIELD_CORE(core) ss_control_test_sgi(core)
#define portYIELD_FROM_ISR(woken) ((void)(woken))
#define portSET_INTERRUPT_MASK() ss_control_test_mask()
#define portCLEAR_INTERRUPT_MASK(mask) ss_control_test_unmask(mask)
#endif
