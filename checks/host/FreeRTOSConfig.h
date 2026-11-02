#ifndef HOST_FREERTOS_CONFIG_H
#define HOST_FREERTOS_CONFIG_H
/* Kernel configuration of the host shim. It matches include/FreeRTOSConfig.h in
 * every setting that changes scheduling (preemption, time slicing, 1 kHz tick,
 * eight priorities, 64-bit ticks, static allocation, task notification array of two). */
#ifndef __ASSEMBLER__
void host_assert_failed(const char *file, unsigned line) __attribute__((noreturn));
void vApplicationIdleHook(void);
void host_trace_give(void);
#endif
#define configUSE_PREEMPTION 1
#define configUSE_IDLE_HOOK 1
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
#define configCHECK_FOR_STACK_OVERFLOW 0
#define traceTASK_NOTIFY_GIVE_FROM_ISR(index) host_trace_give()
#define configASSERT(condition) do { if (!(condition)) host_assert_failed(__FILE__, __LINE__); } while (0)
#define INCLUDE_vTaskDelay 1
#define INCLUDE_vTaskDelete 1
#define INCLUDE_vTaskSuspend 1
#define INCLUDE_xTaskGetSchedulerState 1
#define INCLUDE_xTaskGetCurrentTaskHandle 1
#define INCLUDE_xTaskGetIdleTaskHandle 1
#define INCLUDE_uxTaskGetStackHighWaterMark 0
#define INCLUDE_vTaskPrioritySet 1
#define INCLUDE_uxTaskPriorityGet 1
#define INCLUDE_eTaskGetState 1
#define INCLUDE_xTaskAbortDelay 1
#endif
