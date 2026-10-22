/* Idle-window workload for checks/cpu-power-states.py.
 *
 * The check builds the cached eight-core image with this file in place of
 * platform/a733-smp-cached/diagnostic.c. One task per secondary core blocks
 * on its notification. The core 0 task then opens a 64-tick window in which
 * no task is ready and notifies every secondary task. Each secondary task
 * then delays two ticks, so core 0's tick handler also wakes it. The core 0
 * task exits once every secondary task has completed both wake-ups on its
 * own core. a733_cpu_power_phase tells the instruction model where
 * the window starts and ends. */
#include "FreeRTOS.h"
#include "task.h"
#include "platform.h"
#include "boot.h"

volatile uint64_t a733_cpu_power_phase __attribute__((used));
static StaticTask_t tasks[8];
static StackType_t stacks[8][2048];
static TaskHandle_t handles[8];
static uint32_t started[8], woken[8], delayed[8];

static int all_set(const uint32_t *flags)
{
    for (unsigned c = 1; c < 8; ++c)
        if (!__atomic_load_n(&flags[c], __ATOMIC_ACQUIRE)) return 0;
    return 1;
}

static void secondary(void *argument)
{
    unsigned c = (uintptr_t)argument;
    __atomic_store_n(&started[c], 1, __ATOMIC_RELEASE);
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        configASSERT(portGET_CORE_ID() == c);
        __atomic_store_n(&woken[c], 1, __ATOMIC_RELEASE);
        vTaskDelay(2);
        configASSERT(portGET_CORE_ID() == c);
        __atomic_store_n(&delayed[c], 1, __ATOMIC_RELEASE);
    }
}

static void primary(void *argument)
{
    (void)argument;
    for (unsigned tick = 0; !all_set(started); ++tick) {
        configASSERT(tick < 8);
        vTaskDelay(1);
    }
    /* The instruction model ends a core's quantum at each WFE of a kernel
     * lock wait, so a contended block can take several ticks. Eight ticks let
     * every secondary task reach its blocking wait before the window opens;
     * the check fails if one has not. */
    vTaskDelay(8);
    __atomic_store_n(&a733_cpu_power_phase, 1, __ATOMIC_RELEASE);
    vTaskDelay(64);
    __atomic_store_n(&a733_cpu_power_phase, 2, __ATOMIC_RELEASE);
    for (unsigned c = 1; c < 8; ++c) xTaskNotifyGive(handles[c]);
    for (unsigned tick = 0; !all_set(woken) || !all_set(delayed); ++tick) {
        configASSERT(tick < 8);
        vTaskDelay(1);
    }
    __atomic_store_n(&a733_cpu_power_phase, 3, __ATOMIC_RELEASE);
    platform_exit(0);
}

void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
    (void)task;
    platform_panic(name, 2);
}

void a733_smp_diagnostic_start(void)
{
    handles[0] = xTaskCreateStaticAffinitySet(primary, "primary", 2048, NULL, 3,
        stacks[0], &tasks[0], 1UL);
    configASSERT(handles[0]);
    for (unsigned c = 1; c < 8; ++c) {
        handles[c] = xTaskCreateStaticAffinitySet(secondary, "secondary", 2048,
            (void *)(uintptr_t)c, 2, stacks[c], &tasks[c], 1UL << c);
        configASSERT(handles[c]);
    }
    vTaskStartScheduler();
    platform_panic("scheduler returned", 3);
}
