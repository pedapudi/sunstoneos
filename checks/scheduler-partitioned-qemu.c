#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "platform.h"
#include <stdint.h>

_Static_assert(configSS_PARTITIONED_SCHEDULER == 1, "exercise the private fork");
_Static_assert(configNUMBER_OF_CORES == 8, "exercise every owner");
#define ROUNDS 64U
#define VICTIMS 32U
#define WITHHOLD_ROUNDS 16U
static StaticTask_t worker_state[8], busy_state[8], mover_state[2], control_state, victim_state[VICTIMS];
static StackType_t worker_stack[8][2048], busy_stack[8][1024], mover_stack[2][2048], control_stack[4096], victim_stack[VICTIMS][1024];
static TaskHandle_t workers[8], movers[2], backgrounds[8], controller, local_sender, local_receiver, home_blocker;
static StaticTask_t local_state[3];
static StackType_t local_stack[3][2048];
static StaticQueue_t reply_state, timeout_state;
static uint8_t reply_storage[32 * sizeof(uint32_t)], timeout_storage[sizeof(uint32_t)];
static QueueHandle_t replies, empty;
static uint64_t busy[8], switches[8], idle_selected[8], scheduling_locks[8];
static unsigned in_switch[8], worker_round[8], mover_round[2], mover_mask[2], victim_runs;
/* These symbols also identify the stopped workload in a QEMU GDB snapshot. */
static unsigned control_round, control_phase, isolated_local, selected_idle[8];
static unsigned withholding, blocker_release;
static uint64_t withholding_completed, withholding_observations, withholding_ticks;
static uint64_t local_phase_locks, local_wakes, local_completed, local_ticks, peer_idle_samples;
extern BaseType_t __real_xTaskGenericNotify(TaskHandle_t, UBaseType_t, uint32_t, eNotifyAction, uint32_t *);
BaseType_t __wrap_xTaskGenericNotify(TaskHandle_t task, UBaseType_t index, uint32_t value,
                                    eNotifyAction action, uint32_t *previous)
{
    unsigned cpu = portGET_CORE_ID();
    BaseType_t result = __real_xTaskGenericNotify(task, index, value, action, previous);
    if (__atomic_load_n(&isolated_local, __ATOMIC_RELAXED) && cpu == 1 && task == local_receiver)
        __atomic_fetch_add(&local_wakes, 1, __ATOMIC_RELAXED);
    return result;
}
extern uint64_t check_fp_context(uint64_t pattern);
extern void __real_vTaskSwitchContext(BaseType_t cpu);
extern void __real_vPortRecursiveLock(uint8_t cpu, ePortRTOSLock lock, BaseType_t acquire);
void __wrap_vTaskSwitchContext(BaseType_t cpu)
{
    in_switch[cpu] = 1;
    __real_vTaskSwitchContext(cpu);
    in_switch[cpu] = 0;
    __atomic_fetch_add(&switches[cpu], 1, __ATOMIC_RELAXED);
    unsigned idle = uxTaskPriorityGet(NULL) == 0;
    __atomic_store_n(&selected_idle[cpu], idle, __ATOMIC_RELAXED);
    if (idle) __atomic_fetch_add(&idle_selected[cpu], 1, __ATOMIC_RELAXED);
}
void __wrap_vPortRecursiveLock(uint8_t cpu, ePortRTOSLock lock, BaseType_t acquire)
{
    if (acquire && in_switch[cpu]) __atomic_fetch_add(&scheduling_locks[cpu], 1, __ATOMIC_RELAXED);
    if (acquire && cpu == 1 && __atomic_load_n(&isolated_local, __ATOMIC_RELAXED))
        __atomic_fetch_add(&local_phase_locks, 1, __ATOMIC_RELAXED);
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
static void local_receive(void *unused)
{
    (void)unused;
    for (;;) {
        configASSERT(ulTaskNotifyTake(pdTRUE, portMAX_DELAY) == 1);
        configASSERT(portGET_CORE_ID() == 1);
        if (__atomic_load_n(&withholding, __ATOMIC_ACQUIRE)) {
            ++withholding_completed;
            xTaskNotifyGive(controller);
        } else {
            __atomic_fetch_add(&local_completed, 1, __ATOMIC_RELAXED);
            xTaskNotifyGive(local_sender);
        }
    }
}
static void local_send(void *unused)
{
    (void)unused;
    configASSERT(ulTaskNotifyTake(pdTRUE, portMAX_DELAY) == 1);
    TickType_t start = xTaskGetTickCount();
    __atomic_store_n(&isolated_local, 1, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < ROUNDS; ++i) {
        configASSERT(portGET_CORE_ID() == 1);
        /* CPU1 has a continuously runnable background task. CPU2 is eligible
         * for the receiver but idle; the receiver retains its admitted home. */
        configASSERT(__atomic_load_n(&selected_idle[2], __ATOMIC_RELAXED));
        ++peer_idle_samples;
#ifdef SS_QEMU_LOCK_NEGATIVE_CONTROL
        if (!i) {
            UBaseType_t mask = portSET_INTERRUPT_MASK();
            vPortRecursiveLock(1, eTaskLock, pdTRUE);
            vPortRecursiveLock(1, eTaskLock, pdFALSE);
            portCLEAR_INTERRUPT_MASK(mask);
        }
#endif
        xTaskNotifyGive(local_receiver);
        configASSERT(ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000)) == 1);
        vTaskDelay(1);
    }
    local_ticks = xTaskGetTickCount() - start;
    __atomic_store_n(&isolated_local, 0, __ATOMIC_RELEASE);
    xTaskNotifyGive(controller);
    vTaskSuspend(NULL);
    for (;;) {}
}
static void withhold_home(void *unused)
{
    (void)unused;
    for (;;) {
        configASSERT(ulTaskNotifyTake(pdTRUE, portMAX_DELAY) == 1);
        configASSERT(portGET_CORE_ID() == 1);
        xTaskNotifyGive(controller);
        while (!__atomic_load_n(&blocker_release, __ATOMIC_ACQUIRE))
            __asm__ volatile("yield");
    }
}
static void control(void *unused)
{
    (void)unused;
    for (unsigned round = 0; round < ROUNDS; ++round) {
        control_round = round;
        control_phase = 1;
        for (unsigned id = 0; id < 8; ++id) xTaskNotifyGive(workers[id]);
        for (unsigned id = 0; id < 2; ++id) {
            unsigned core = (round + 4 * id) % 8;
            unsigned mask = (1U << core) | (round % 2 ? 0 : 1U << ((core + 1) % 8));
            __atomic_store_n(&mover_mask[id], mask, __ATOMIC_RELEASE);
            vTaskCoreAffinitySet(movers[id], mask);
            xTaskNotifyGive(movers[id]);
        }
        control_phase = 2;
        unsigned seen = 0;
        for (unsigned id = 0; id < 10; ++id) {
            unsigned value = response();
            configASSERT(value < 10 && !(seen & (1U << value)));
            seen |= 1U << value;
        }
        configASSERT(seen == 1023);
        control_phase = 3;
        if (round < VICTIMS) {
            TaskHandle_t handle = xTaskCreateStaticAffinitySet(victim, "delete-pending", 1024, NULL, 4,
                victim_stack[round], &victim_state[round], 1U << (1 + round % 7));
            configASSERT(handle && response() == 16);
            /* The wake and remote delete compete with the target's owner drain. */
            xTaskNotifyGive(handle);
            vTaskDelete(handle);
        }
    }
    control_phase = 5;
    for (unsigned id = 2; id < 8; ++id) vTaskSuspend(backgrounds[id]);
    local_receiver = xTaskCreateStaticAffinitySet(local_receive, "local-receiver", 2048,
        NULL, 4, local_stack[0], &local_state[0], 2);
    local_sender = xTaskCreateStaticAffinitySet(local_send, "local-sender", 2048,
        NULL, 2, local_stack[1], &local_state[1], 2);
    configASSERT(local_receiver && local_sender);
    vTaskCoreAffinitySet(local_receiver, 6);
    vTaskDelay(4);
    uint64_t busy_start = __atomic_load_n(&busy[1], __ATOMIC_RELAXED);
    xTaskNotifyGive(local_sender);
    configASSERT(ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000)) == 1);
    uint64_t busy_delta = __atomic_load_n(&busy[1], __ATOMIC_RELAXED) - busy_start;
    platform_puts("local-wake attempts="); platform_hex(local_wakes);
    platform_puts(" completions="); platform_hex(local_completed);
    platform_puts(" ticks="); platform_hex(local_ticks);
    platform_puts(" tick-hz="); platform_hex(configTICK_RATE_HZ);
    platform_puts(" busy-home-progress="); platform_hex(busy_delta);
    platform_puts(" idle-peer-samples="); platform_hex(peer_idle_samples);
    platform_puts(" all-global-locks="); platform_hex(local_phase_locks);
    platform_puts("\n");
    configASSERT(local_wakes == ROUNDS && local_completed == ROUNDS);
    configASSERT(peer_idle_samples == ROUNDS && busy_delta > 0);
    configASSERT(local_ticks >= ROUNDS && local_ticks < pdMS_TO_TICKS(1000));
    control_phase = 6;
    configASSERT(local_phase_locks == 0);
    home_blocker = xTaskCreateStaticAffinitySet(withhold_home, "home-blocker", 2048,
        NULL, 5, local_stack[2], &local_state[2], 2);
    configASSERT(home_blocker);
    __atomic_store_n(&withholding, 1, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < WITHHOLD_ROUNDS; ++i) {
        __atomic_store_n(&blocker_release, 0, __ATOMIC_RELEASE);
        xTaskNotifyGive(home_blocker);
        configASSERT(ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000)) == 1);
        TickType_t start = xTaskGetTickCount();
        xTaskNotifyGive(local_receiver);
        vTaskDelay(1);
        /* One counted opportunity requires all three states simultaneously:
         * receiver READY, higher-priority home blocker CURRENT, allowed peer idle. */
        configASSERT(eTaskGetState(local_receiver) == eReady);
        configASSERT(xTaskGetCurrentTaskHandleForCore(1) == home_blocker);
        configASSERT(__atomic_load_n(&selected_idle[2], __ATOMIC_RELAXED));
        ++withholding_observations;
        configASSERT(withholding_completed == i);
        __atomic_store_n(&blocker_release, 1, __ATOMIC_RELEASE);
        configASSERT(ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000)) == 1);
        withholding_ticks += xTaskGetTickCount() - start;
    }
    platform_puts("withheld-ready opportunities="); platform_hex(WITHHOLD_ROUNDS);
    platform_puts(" stranded-observations="); platform_hex(withholding_observations);
    platform_puts(" home-completions="); platform_hex(withholding_completed);
    platform_puts(" elapsed-ticks="); platform_hex(withholding_ticks);
    platform_puts(" home-mask="); platform_hex(6);
    platform_puts(" blocker-priority="); platform_hex(5);
    platform_puts(" receiver-priority="); platform_hex(4);
    platform_puts("\n");
    configASSERT(withholding_observations == WITHHOLD_ROUNDS && withholding_completed == WITHHOLD_ROUNDS);
    for (unsigned id = 0; id < 8; ++id) {
        configASSERT(__atomic_load_n(&worker_round[id], __ATOMIC_RELAXED) == ROUNDS);
        configASSERT(__atomic_load_n(&busy[id], __ATOMIC_RELAXED) > 0);
        configASSERT(__atomic_load_n(&switches[id], __ATOMIC_RELAXED) > 0);
        platform_puts("owner cpu="); platform_hex(id);
        platform_puts(" switches="); platform_hex(switches[id]);
        platform_puts(" idle-selections="); platform_hex(idle_selected[id]);
        platform_puts(" busy-progress="); platform_hex(busy[id]);
        platform_puts(" switch-path-global-locks="); platform_hex(scheduling_locks[id]);
        platform_puts("\n");
    }
    for (unsigned id = 0; id < 2; ++id) configASSERT(mover_round[id] == ROUNDS);
    configASSERT(local_phase_locks == 0);
    for (unsigned id = 0; id < 8; ++id)
        configASSERT(__atomic_load_n(&scheduling_locks[id], __ATOMIC_RELAXED) == 0);
    control_phase = 7;
    platform_puts("PASS partitioned concurrent: 8 owners, pinned and multiple-affinity priorities, notification/block/timeout, FP migration, pending delete, zero isolated local-wake/switch global locks\n");
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
        backgrounds[id] = xTaskCreateStaticAffinitySet(background, "busy", 1024, (void *)(uintptr_t)id,
            1, busy_stack[id], &busy_state[id], 1U << id);
        configASSERT(backgrounds[id]);
    }
    for (unsigned id = 0; id < 2; ++id) {
        mover_mask[id] = 255;
        movers[id] = xTaskCreateStaticAffinitySet(mover, "mover", 2048, (void *)(uintptr_t)id,
            3, mover_stack[id], &mover_state[id], 255);
        configASSERT(movers[id]);
    }
    controller = xTaskCreateStaticAffinitySet(control, "control", 4096, NULL, 5, control_stack, &control_state, 1);
    configASSERT(controller);
    vTaskStartScheduler();
    platform_panic("scheduler returned", 0);
}
