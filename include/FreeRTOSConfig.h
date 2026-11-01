#ifndef SUNSTONE_FREERTOS_CONFIG_H
#define SUNSTONE_FREERTOS_CONFIG_H

#ifndef __ASSEMBLER__
#ifdef __cplusplus
extern "C" {
#define _Noreturn [[noreturn]]
#endif
#ifdef SUNSTONE_GPU_TIMING
void gpu_queue_timing_block_receive(void *queue);
#define traceBLOCKING_ON_QUEUE_RECEIVE(queue) gpu_queue_timing_block_receive((void *)(queue))
#endif
void platform_tick_setup(void);
void platform_tick_ack(void);
_Noreturn void platform_panic(const char *file, unsigned line);
#ifdef __cplusplus
#undef _Noreturn
}
#endif
#endif

#ifdef SUNSTONE_NEWLIB
#define configUSE_NEWLIB_REENTRANT 1
#define INCLUDE_xTaskGetSchedulerState 1
#define INCLUDE_xTaskGetCurrentTaskHandle 1
#endif

#define configUSE_PREEMPTION 1
/* The private fork is under conversion. Production targets keep the control
 * kernel until every enabled task API and the owner port hooks are admitted. */
#ifndef configSS_PARTITIONED_SCHEDULER
#define configSS_PARTITIONED_SCHEDULER 0
#endif
#if defined(SUNSTONE_LATENCY) && !defined(__ASSEMBLER__)
#include "sunstone_latency.h"
#define tracePOST_MOVED_TASK_TO_READY_STATE(task) ss_latency_task_ready(task)
#define INCLUDE_xTaskGetIdleTaskHandle 1
#ifndef INCLUDE_xTaskGetCurrentTaskHandle
#define INCLUDE_xTaskGetCurrentTaskHandle 1
#endif
#endif
#define configUSE_IDLE_HOOK 0
#define configUSE_TICK_HOOK 0
#define configUSE_TIME_SLICING 1
#define configCPU_CLOCK_HZ 1000000000UL
#define configTICK_RATE_HZ 1000
#define configMAX_PRIORITIES 8
#define configMINIMAL_STACK_SIZE 512
#define configMAX_TASK_NAME_LEN 16
#define configUSE_16_BIT_TICKS 0
#define configIDLE_SHOULD_YIELD 1
#define configSUPPORT_STATIC_ALLOCATION 1
#define configSUPPORT_DYNAMIC_ALLOCATION 0
#define configKERNEL_PROVIDED_STATIC_MEMORY 1
#define configUSE_MUTEXES 1
#define configUSE_RECURSIVE_MUTEXES 0
#define configUSE_COUNTING_SEMAPHORES 1
#define configUSE_TIMERS 0
#define configUSE_TASK_NOTIFICATIONS 1
#define configTASK_NOTIFICATION_ARRAY_ENTRIES 2
#define configCHECK_FOR_STACK_OVERFLOW 2
#define configUSE_TASK_FPU_SUPPORT 2
#ifdef SUNSTONE_SECURE_MONITOR
// GICv3 exposes 16 nonsecure priorities when the secure view implements 32.
#define configUNIQUE_INTERRUPT_PRIORITIES 16
#define configMAX_API_CALL_INTERRUPT_PRIORITY 11ULL
#else
#define configUNIQUE_INTERRUPT_PRIORITIES 32
#define configMAX_API_CALL_INTERRUPT_PRIORITY 19ULL
#endif
#define configSETUP_TICK_INTERRUPT() platform_tick_setup()
#define configCLEAR_TICK_INTERRUPT() platform_tick_ack()
#define configASSERT(condition) do { if (!(condition)) platform_panic(__FILE__, __LINE__); } while (0)
#define INCLUDE_vTaskDelay 1
#define INCLUDE_vTaskSuspend 1

#endif
