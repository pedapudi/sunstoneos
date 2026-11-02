#ifndef HOST_PORTMACRO_H
#define HOST_PORTMACRO_H
/* Port of the unmodified FreeRTOS kernel to a deterministic host model: the
 * board's 64-bit tick, base and unsigned types, tasks as user-level contexts
 * that switch only inside the kernel's own yield points, and interrupts that the
 * script raises. See checks/host/host_rtos.h. */
#include <limits.h>
#include <stdint.h>
#define portCHAR char
#define portFLOAT float
#define portDOUBLE double
#define portLONG long
#define portSHORT short
#define portSTACK_TYPE uint64_t
#define portBASE_TYPE long
#define portPOINTER_SIZE_TYPE uintptr_t
typedef portSTACK_TYPE StackType_t;
typedef long BaseType_t;
typedef uint64_t UBaseType_t;
typedef uint64_t TickType_t;
#define portMAX_DELAY ((TickType_t)0xffffffffffffffffULL)
#define portTICK_TYPE_IS_ATOMIC 1
#define portSTACK_GROWTH (-1)
#define portHAS_STACK_OVERFLOW_CHECKING 0
#define portTICK_PERIOD_MS ((TickType_t)1000 / configTICK_RATE_HZ)
#define portBYTE_ALIGNMENT 16
extern unsigned host_core_id;
#define portGET_CORE_ID() host_core_id
extern void vPortYield(void);
#define portYIELD() vPortYield()
#define portEND_SWITCHING_ISR(x) do { if ((x) != pdFALSE) host_isr_request_switch(); } while (0)
#define portYIELD_FROM_ISR(x) portEND_SWITCHING_ISR(x)
extern void host_isr_request_switch(void);
extern UBaseType_t xPortSetInterruptMask(void);
extern void vPortClearInterruptMask(UBaseType_t);
extern void vPortEnterCritical(void);
extern void vPortExitCritical(void);
extern void vPortDisableInterrupts(void);
extern void vPortEnableInterrupts(void);
#define portSET_INTERRUPT_MASK_FROM_ISR() xPortSetInterruptMask()
#define portCLEAR_INTERRUPT_MASK_FROM_ISR(x) vPortClearInterruptMask(x)
#define portDISABLE_INTERRUPTS() vPortDisableInterrupts()
#define portENABLE_INTERRUPTS() vPortEnableInterrupts()
#define portENTER_CRITICAL() vPortEnterCritical()
#define portEXIT_CRITICAL() vPortExitCritical()
extern void host_task_dying(void *);
#define portPRE_TASK_DELETE_HOOK(task, pend) host_task_dying(task)
#define portTASK_FUNCTION_PROTO(f, p) void f(void *p)
#define portTASK_FUNCTION(f, p) void f(void *p)
#define portMEMORY_BARRIER() __asm volatile("" ::: "memory")
#define portNOP() __asm volatile("")
#endif
