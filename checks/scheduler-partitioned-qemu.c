#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "platform.h"
#include <stdint.h>

_Static_assert(configSS_PARTITIONED_SCHEDULER == 1, "exercise the private fork");
_Static_assert(configNUMBER_OF_CORES == 8, "exercise every owner");
#define ROUNDS 64U
#define VICTIMS 32U
static StaticTask_t worker_state[8], busy_state[8], mover_state[2], control_state, victim_state[VICTIMS];
static StackType_t worker_stack[8][2048], busy_stack[8][1024], mover_stack[2][2048], control_stack[4096], victim_stack[VICTIMS][1024];
static TaskHandle_t workers[8], movers[2];
static StaticQueue_t reply_state, timeout_state;
static uint8_t reply_storage[32 * sizeof(uint32_t)], timeout_storage[sizeof(uint32_t)];
static QueueHandle_t replies, empty;
static uint64_t busy[8], switches[8], idle_selected[8], scheduling_locks[8];
static unsigned in_switch[8], worker_round[8], mover_round[2], mover_mask[2], victim_runs;
extern uint64_t check_fp_context(uint64_t pattern);
extern void __real_vTaskSwitchContext(BaseType_t cpu);
extern void __real_vPortRecursiveLock(uint8_t cpu, ePortRTOSLock lock, BaseType_t acquire);
void __wrap_vTaskSwitchContext(BaseType_t cpu)
{
    in_switch[cpu] = 1;
    __real_vTaskSwitchContext(cpu);
    in_switch[cpu] = 0;
    __atomic_fetch_add(&switches[cpu], 1, __ATOMIC_RELAXED);
    if (uxTaskPriorityGet(NULL) == 0) __atomic_fetch_add(&idle_selected[cpu], 1, __ATOMIC_RELAXED);
}
void __wrap_vPortRecursiveLock(uint8_t cpu, ePortRTOSLock lock, BaseType_t acquire)
{
    if (acquire && in_switch[cpu]) __atomic_fetch_add(&scheduling_locks[cpu], 1, __ATOMIC_RELAXED);
    __real_vPortRecursiveLock(cpu, lock, acquire);
}
static void respond(uint32_t value)
{
    configASSERT(xQueueSend(replies, &value, pdMS_TO_TICKS(1000)) == pdPASS);
}
static uint32_t response(void)
{
    uint32_t value;
    configASSERT(xQueueReceive(replies, &value, pdMS_TO_TICKS(2000)) == pdPASS);
    return value;
}
static void worker(void *argument)
{
    unsigned id = (uintptr_t)argument;
    for (;;) {
        configASSERT(ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000)) == 1);
        configASSERT(portGET_CORE_ID() == id);
        uint32_t value;
        configASSERT(xQueueReceive(empty, &value, 1) == pdFAIL);
        __atomic_fetch_add(&worker_round[id], 1, __ATOMIC_RELAXED);
        respond(id);
    }
}
static void background(void *argument)
{
    unsigned id = (uintptr_t)argument;
    for (;;) {
        configASSERT(portGET_CORE_ID() == id);
        __atomic_fetch_add(&busy[id], 1, __ATOMIC_RELAXED);
    }
}
static void mover(void *argument)
{
    unsigned id = (uintptr_t)argument;
    for (;;) {
        configASSERT(ulTaskNotifyTake(pdTRUE, portMAX_DELAY) == 1);
        unsigned core = portGET_CORE_ID();
        configASSERT(__atomic_load_n(&mover_mask[id], __ATOMIC_ACQUIRE) & (1U << core));
        configASSERT(check_fp_context(0xafa0000000000000ULL + id));
        __atomic_fetch_add(&mover_round[id], 1, __ATOMIC_RELAXED);
        respond(8 + id);
    }
}
static void victim(void *unused)
{
    (void)unused;
    respond(16);
    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    __atomic_fetch_add(&victim_runs, 1, __ATOMIC_RELAXED);
    vTaskSuspend(NULL);
    for (;;) {}
}
static void control(void *unused)
{
    (void)unused;
    for (unsigned round = 0; round < ROUNDS; ++round) {
        for (unsigned id = 0; id < 8; ++id) xTaskNotifyGive(workers[id]);
        for (unsigned id = 0; id < 2; ++id) {
            unsigned core = (round + 4 * id) % 8;
            unsigned mask = (1U << core) | (round % 2 ? 0 : 1U << ((core + 1) % 8));
            __atomic_store_n(&mover_mask[id], mask, __ATOMIC_RELEASE);
            vTaskCoreAffinitySet(movers[id], mask);
            xTaskNotifyGive(movers[id]);
        }
        unsigned seen = 0;
        for (unsigned id = 0; id < 10; ++id) {
            unsigned value = response();
            configASSERT(value < 10 && !(seen & (1U << value)));
            seen |= 1U << value;
        }
        configASSERT(seen == 1023);
        if (round < VICTIMS) {
            TaskHandle_t handle = xTaskCreateStaticAffinitySet(victim, "delete-pending", 1024, NULL, 4,
                victim_stack[round], &victim_state[round], 1U << (1 + round % 7));
            configASSERT(handle && response() == 16);
            /* The wake and remote delete compete with the target's owner drain. */
            xTaskNotifyGive(handle);
            vTaskDelete(handle);
        }
    }
    for (unsigned id = 0; id < 8; ++id) {
        configASSERT(__atomic_load_n(&worker_round[id], __ATOMIC_RELAXED) == ROUNDS);
        configASSERT(__atomic_load_n(&busy[id], __ATOMIC_RELAXED) > 0);
        configASSERT(__atomic_load_n(&switches[id], __ATOMIC_RELAXED) > 0);
        configASSERT(__atomic_load_n(&scheduling_locks[id], __ATOMIC_RELAXED) == 0);
        platform_puts("owner cpu="); platform_hex(id);
        platform_puts(" switches="); platform_hex(switches[id]);
        platform_puts(" idle-selections="); platform_hex(idle_selected[id]);
        platform_puts(" busy-progress="); platform_hex(busy[id]);
        platform_puts(" scheduling-global-locks="); platform_hex(scheduling_locks[id]);
        platform_puts("\n");
    }
    for (unsigned id = 0; id < 2; ++id) configASSERT(mover_round[id] == ROUNDS);
    platform_puts("PASS partitioned concurrent: 8 owners, pinned and multiple-affinity priorities, notification/block/timeout, FP migration, pending delete, zero local-switch global locks\n");
    platform_exit(0);
}
void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
    (void)task; platform_panic(name, 0);
}
void kernel_main(void)
{
    platform_init();
    replies = xQueueCreateStatic(32, sizeof(uint32_t), reply_storage, &reply_state);
    empty = xQueueCreateStatic(1, sizeof(uint32_t), timeout_storage, &timeout_state);
    configASSERT(replies && empty);
    for (unsigned id = 0; id < 8; ++id) {
        workers[id] = xTaskCreateStaticAffinitySet(worker, "worker", 2048, (void *)(uintptr_t)id,
            4, worker_stack[id], &worker_state[id], 1U << id);
        configASSERT(workers[id]);
        configASSERT(xTaskCreateStaticAffinitySet(background, "busy", 1024, (void *)(uintptr_t)id,
            1, busy_stack[id], &busy_state[id], 1U << id));
    }
    for (unsigned id = 0; id < 2; ++id) {
        mover_mask[id] = 255;
        movers[id] = xTaskCreateStaticAffinitySet(mover, "mover", 2048, (void *)(uintptr_t)id,
            3, mover_stack[id], &mover_state[id], 255);
        configASSERT(movers[id]);
    }
    configASSERT(xTaskCreateStaticAffinitySet(control, "control", 4096, NULL, 5, control_stack, &control_state, 1));
    vTaskStartScheduler();
    platform_panic("scheduler returned", 0);
}
