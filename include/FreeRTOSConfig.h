#ifndef SUNSTONE_FREERTOS_CONFIG_H
#define SUNSTONE_FREERTOS_CONFIG_H

#ifndef __ASSEMBLER__
void platform_tick_setup(void);
void platform_tick_ack(void);
_Noreturn void platform_panic(const char *file, unsigned line);
#endif

#define configUSE_PREEMPTION 1
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
