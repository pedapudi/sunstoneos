#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "platform.h"
#include <stdint.h>

#ifdef SS_QEMU_DYNAMIC_LIFECYCLE
_Static_assert(configSS_PARTITIONED_SCHEDULER == 1, "private admission");
_Static_assert(configSUPPORT_DYNAMIC_ALLOCATION == 1, "real heap lifecycle");
static StaticTask_t lifecycle_control_state, reusable_state;
static StackType_t lifecycle_control_stack[4096], reusable_stack[1024];
static TaskHandle_t lifecycle_controller;
static unsigned heap_children, self_children, static_children;
static void lifecycle_child(void *argument)
{
    unsigned self_delete = (uintptr_t)argument;
    configASSERT(portGET_CORE_ID() == 1);
    xTaskNotifyGive(lifecycle_controller);
    if (self_delete) vTaskDelete(NULL);
    else (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    platform_panic("deleted child resumed", 0);
}
static void wait_child(void)
{
    configASSERT(ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000)) == 1);
}
static void lifecycle_control(void *unused)
{
    (void)unused;
    void *warm = pvPortMalloc(1);
    configASSERT(warm);
    vPortFree(warm);
    size_t baseline = xPortGetFreeHeapSize();
    for (unsigned round = 0; round < 8; ++round) {
        TaskHandle_t task = NULL;
        configASSERT(xTaskCreateAffinitySet(lifecycle_child, "heap-child", 1024,
            NULL, 3, 2, &task) == pdPASS && task);
        wait_child();
        vTaskDelete(task);
        configASSERT(xPortGetFreeHeapSize() == baseline);
        ++heap_children;
        for (unsigned reuse = 0; reuse < 2; ++reuse) {
            task = xTaskCreateStaticAffinitySet(lifecycle_child, "static-reuse", 1024,
                NULL, 3, reusable_stack, &reusable_state, 2);
            configASSERT(task);
            wait_child();
            vTaskDelete(task);
            ++static_children;
        }
        configASSERT(xTaskCreateAffinitySet(lifecycle_child, "self-delete", 1024,
            (void *)(uintptr_t)1, 3, 2, &task) == pdPASS && task);
        wait_child();
        TickType_t start = xTaskGetTickCount();
        while (xPortGetFreeHeapSize() != baseline && xTaskGetTickCount() - start < pdMS_TO_TICKS(1000))
            vTaskDelay(1);
        configASSERT(xPortGetFreeHeapSize() == baseline);
        ++self_children;
    }
    TaskHandle_t failed = (TaskHandle_t)(uintptr_t)1;
    configASSERT(xTaskCreateAffinitySet(lifecycle_child, "allocation-fail",
        configTOTAL_HEAP_SIZE, NULL, 3, 2, &failed) == errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY);
    configASSERT(failed == NULL && xPortGetFreeHeapSize() == baseline);
    platform_puts("heap lifecycle nonself-deletes="); platform_hex(heap_children);
    platform_puts(" static-reuses="); platform_hex(static_children);
    platform_puts(" self-deletes="); platform_hex(self_children);
    platform_puts(" restored-free-bytes="); platform_hex(baseline);
    platform_puts("\nPASS partitioned heap lifecycle: dynamic admission, upstream heap4 free, immediate static reuse, self-delete idle cleanup and allocation failure\n");
    platform_exit(0);
}
void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
    (void)task; platform_panic(name, 0);
}
void kernel_main(void)
{
    platform_init();
    lifecycle_controller = xTaskCreateStaticAffinitySet(lifecycle_control, "heap-control", 4096,
        NULL, 5, lifecycle_control_stack, &lifecycle_control_state, 1);
    configASSERT(lifecycle_controller);
    vTaskStartScheduler();
    platform_panic("scheduler returned", 0);
}
#else
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
static uint64_t admin_locks[8], switch_admin_locks[8];
static unsigned in_switch[8], worker_round[8], mover_round[2], mover_mask[2], victim_runs;
/* These symbols also identify the stopped workload in a QEMU GDB snapshot. */
static volatile unsigned control_round, control_phase;
static unsigned isolated_local, selected_idle[8];
static unsigned withholding, blocker_release, withhold_cores, blocker_parked;
static uint64_t withholding_completed, withholding_observations, withholding_ticks;
static uint64_t local_phase_locks, local_wakes, local_completed, local_ticks, peer_idle_samples;
void ss_kernel_test_admin_enter(unsigned cpu)
{
    __atomic_fetch_add(&admin_locks[cpu], 1, __ATOMIC_RELAXED);
    if (in_switch[cpu]) {
        __atomic_fetch_add(&switch_admin_locks[cpu], 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&scheduling_locks[cpu], 1, __ATOMIC_RELAXED);
    }
    if (cpu == 1 && __atomic_load_n(&isolated_local, __ATOMIC_RELAXED))
        __atomic_fetch_add(&local_phase_locks, 1, __ATOMIC_RELAXED);
}
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
extern TaskHandle_t volatile pxCurrentTCBs[];
extern char __bss_start[], __bss_end[];
void __wrap_vTaskSwitchContext(BaseType_t cpu)
{
    /* Every context save has just stored the outgoing task's SP_EL0 in the first word of its
     * control block. It must lie in a task stack, which here is static data, and be aligned. */
    uintptr_t saved = *(uintptr_t *)pxCurrentTCBs[cpu];
    configASSERT(saved >= (uintptr_t)__bss_start && saved < (uintptr_t)__bss_end && !(saved & 15));
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
        if (__atomic_load_n(&withholding, __ATOMIC_ACQUIRE)) {
            /* Pulled by the idle eligible peer while the home core runs the blocker. */
            __atomic_fetch_or(&withhold_cores, 1U << portGET_CORE_ID(), __ATOMIC_RELAXED);
            ++withholding_completed;
            xTaskNotifyGive(controller);
        } else {
            configASSERT(portGET_CORE_ID() == 1);
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
        __atomic_store_n(&blocker_parked, 1, __ATOMIC_RELEASE);
        configASSERT(ulTaskNotifyTake(pdTRUE, portMAX_DELAY) == 1);
        __atomic_store_n(&blocker_parked, 0, __ATOMIC_RELEASE);
        configASSERT(portGET_CORE_ID() == 1);
        xTaskNotifyGive(controller);
        while (!__atomic_load_n(&blocker_release, __ATOMIC_ACQUIRE))
            __asm__ volatile("yield");
    }
}
/* Controls applied before the scheduler starts, as the board application does
 * after xTaskCreateStatic (apps/board/main.c, vTaskCoreAffinitySet). No task
 * stack is installed yet and the owners of other cores have not run. Upstream
 * stores the new settings and takes no scheduling action while
 * xSchedulerRunning is false (tasks.c, vTaskCoreAffinitySet, vTaskPrioritySet,
 * vTaskSuspend and vTaskDelete). The settings must take effect at the start. */
#define BOOT_TASKS 5U
static StaticTask_t boot_state[BOOT_TASKS];
static StackType_t boot_stack[BOOT_TASKS][1024];
static TaskHandle_t boot_tasks[BOOT_TASKS];
static unsigned boot_core[BOOT_TASKS] = {99, 99, 99, 99, 99}, boot_runs[BOOT_TASKS];
static void boot_task(void *argument)
{
    unsigned id = (uintptr_t)argument;
    __atomic_store_n(&boot_core[id], portGET_CORE_ID(), __ATOMIC_RELEASE);
    __atomic_fetch_add(&boot_runs[id], 1, __ATOMIC_RELAXED);
    for (;;) vTaskSuspend(NULL);
}
static void boot_controls_verify(void)
{
    /* Task 0: created for any core, moved to core 6 and raised before the start. */
    for (unsigned wait = 0; wait < 200 && __atomic_load_n(&boot_runs[0], __ATOMIC_ACQUIRE) == 0; ++wait) vTaskDelay(1);
    configASSERT(boot_runs[0] == 1 && boot_core[0] == 6);
    configASSERT(vTaskCoreAffinityGet(boot_tasks[0]) == (1U << 6) && uxTaskPriorityGet(boot_tasks[0]) == 4);
    /* Task 1: suspended before the start, so it has not run, and a later resume starts it on core 5. */
    configASSERT(boot_runs[1] == 0 && eTaskGetState(boot_tasks[1]) == eSuspended);
    vTaskResume(boot_tasks[1]);
    for (unsigned wait = 0; wait < 200 && __atomic_load_n(&boot_runs[1], __ATOMIC_ACQUIRE) == 0; ++wait) vTaskDelay(1);
    configASSERT(boot_runs[1] == 1 && boot_core[1] == 5);
    /* Task 2: suspended, then resumed, before the start; it runs once on core 7. */
    for (unsigned wait = 0; wait < 200 && __atomic_load_n(&boot_runs[2], __ATOMIC_ACQUIRE) == 0; ++wait) vTaskDelay(1);
    configASSERT(boot_runs[2] == 1 && boot_core[2] == 7);
    /* Task 4: moved to the boot core above the idle task, as the board's first task is. The
     * boot core owns it, so no acknowledgment can be awaited and no yield may be taken. */
    for (unsigned wait = 0; wait < 200 && __atomic_load_n(&boot_runs[4], __ATOMIC_ACQUIRE) == 0; ++wait) vTaskDelay(1);
    configASSERT(boot_runs[4] == 1 && boot_core[4] == 0);
    /* Task 3: deleted before the start; it never runs. */
    vTaskDelay(4);
    configASSERT(boot_runs[3] == 0);
}
static void control(void *unused)
{
    (void)unused;
    boot_controls_verify();
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
    /* Give the primary idle task a cleanup window after the mixed workload.
     * CPU1 retains its continuously runnable home background task. */
    vTaskSuspend(backgrounds[0]);
    for (unsigned id = 2; id < 8; ++id) vTaskSuspend(backgrounds[id]);
    local_receiver = xTaskCreateStaticAffinitySet(local_receive, "local-receiver", 2048,
        NULL, 4, local_stack[0], &local_state[0], 2);
    local_sender = xTaskCreateStaticAffinitySet(local_send, "local-sender", 2048,
        NULL, 2, local_stack[1], &local_state[1], 2);
    configASSERT(local_receiver && local_sender);
    vTaskCoreAffinitySet(local_receiver, 6);
    /* The suspended background tasks leave their cores to the idle tasks asynchronously. */
    for (unsigned wait = 0; wait < 2000 && !__atomic_load_n(&selected_idle[2], __ATOMIC_RELAXED); ++wait) vTaskDelay(1);
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
        /* The previous round's blocker must have left its spin before the release flag is reused. */
        for (unsigned wait = 0; wait < 1000 && !__atomic_load_n(&blocker_parked, __ATOMIC_ACQUIRE) && i; ++wait)
            vTaskDelay(1);
        __atomic_store_n(&blocker_release, 0, __ATOMIC_RELEASE);
        xTaskNotifyGive(home_blocker);
        configASSERT(ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000)) == 1);
        TickType_t start = xTaskGetTickCount();
        xTaskNotifyGive(local_receiver);
        /* Give the pull a few milliseconds; without it the receiver never runs before the release. */
        for (unsigned wait = 0; wait < 100 && __atomic_load_n(&withholding_completed, __ATOMIC_ACQUIRE) != i + 1; ++wait)
            vTaskDelay(1);
        /* The receiver waits behind its higher-priority home blocker while an allowed
         * peer idles: idle pull must have moved it, and it has run on that peer. */
        if (eTaskGetState(local_receiver) == eReady && __atomic_load_n(&selected_idle[2], __ATOMIC_RELAXED))
            ++withholding_observations; /* stranded */
        configASSERT(xTaskGetCurrentTaskHandleForCore(1) == home_blocker);
        configASSERT(withholding_completed == i + 1);
        configASSERT(ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000)) == 1);
        __atomic_store_n(&blocker_release, 1, __ATOMIC_RELEASE);
        withholding_ticks += xTaskGetTickCount() - start;
        /* Return the receiver to its home owner for the next round. */
        vTaskCoreAffinitySet(local_receiver, 2);
        vTaskCoreAffinitySet(local_receiver, 6);
    }
    platform_puts("withheld-ready opportunities="); platform_hex(WITHHOLD_ROUNDS);
    platform_puts(" stranded-observations="); platform_hex(withholding_observations);
    platform_puts(" home-completions="); platform_hex(withholding_completed);
    platform_puts(" elapsed-ticks="); platform_hex(withholding_ticks);
    platform_puts(" home-mask="); platform_hex(6);
    platform_puts(" blocker-priority="); platform_hex(5);
    platform_puts(" receiver-priority="); platform_hex(4);
    platform_puts("\n");
    configASSERT(withholding_observations == 0 && withholding_completed == WITHHOLD_ROUNDS);
    configASSERT(withhold_cores == (1U << 2)); /* every receiver completion ran on the idle peer */
    for (unsigned id = 0; id < 8; ++id) {
        configASSERT(__atomic_load_n(&worker_round[id], __ATOMIC_RELAXED) == ROUNDS);
        configASSERT(__atomic_load_n(&busy[id], __ATOMIC_RELAXED) > 0);
        configASSERT(__atomic_load_n(&switches[id], __ATOMIC_RELAXED) > 0);
        platform_puts("owner cpu="); platform_hex(id);
        platform_puts(" switches="); platform_hex(__atomic_load_n(&switches[id], __ATOMIC_RELAXED));
        platform_puts(" idle-selections="); platform_hex(__atomic_load_n(&idle_selected[id], __ATOMIC_RELAXED));
        platform_puts(" busy-progress="); platform_hex(__atomic_load_n(&busy[id], __ATOMIC_RELAXED));
        platform_puts(" switch-path-global-locks="); platform_hex(__atomic_load_n(&scheduling_locks[id], __ATOMIC_RELAXED));
        platform_puts(" admin-acquisitions="); platform_hex(__atomic_load_n(&admin_locks[id], __ATOMIC_RELAXED));
        platform_puts(" switch-admin-acquisitions="); platform_hex(__atomic_load_n(&switch_admin_locks[id], __ATOMIC_RELAXED));
        platform_puts("\n");
    }
    uint64_t observed_admin = 0;
    for (unsigned id = 0; id < 8; ++id) observed_admin += __atomic_load_n(&admin_locks[id], __ATOMIC_RELAXED);
    configASSERT(observed_admin > 0);
    for (unsigned id = 0; id < 2; ++id) configASSERT(mover_round[id] == ROUNDS);
    configASSERT(local_phase_locks == 0);
    for (unsigned id = 0; id < 8; ++id)
        configASSERT(__atomic_load_n(&scheduling_locks[id], __ATOMIC_RELAXED) == 0);
    control_phase = 7;
    extern unsigned long ss_kernel_test_continuations(void);
#ifdef SUNSTONE_OWNER_COUNTERS
    {
        extern uint64_t ulSunstoneOwnerCounters(unsigned cpu, unsigned which);
        uint64_t exit_work = 0, skipped = 0, tick_work = 0;
        for (unsigned id = 0; id < 8; ++id) {
            exit_work += ulSunstoneOwnerCounters(id, 0); skipped += ulSunstoneOwnerCounters(id, 1); tick_work += ulSunstoneOwnerCounters(id, 2);
            platform_puts("owner-work cpu="); platform_hex(id); platform_puts(" exit="); platform_hex(ulSunstoneOwnerCounters(id, 0));
            platform_puts(" skipped="); platform_hex(ulSunstoneOwnerCounters(id, 1)); platform_puts(" tick="); platform_hex(ulSunstoneOwnerCounters(id, 2));
            platform_puts(" service="); platform_hex(ulSunstoneOwnerCounters(id, 3)); platform_puts("\n");
        }
        /* Interrupt exits run owner work. In the plain concurrent case none is skipped; in the storm case the
         * control interrupts skip theirs and the ordinary exits keep running. */
        configASSERT(exit_work > 0 && tick_work > 0);
#ifndef SS_STORM
        configASSERT(skipped == 0);
#else
        configASSERT(skipped > 0);
#endif
    }
#endif
    platform_puts("service-continuations="); platform_hex(ss_kernel_test_continuations()); platform_puts("\n");
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
    /* The check can delay the primary core by a loaded count of loop turns before it creates any
     * task. Different delays shift every later interleaving on the deterministic clock; zero
     * (the default) is no delay. checks/scheduler-partitioned-qemu.py --skew loads the word. */
    for (volatile uint32_t turns = *(volatile uint32_t *)0x47ff0000UL * 61u; turns; --turns) {}
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
    for (unsigned id = 0; id < BOOT_TASKS; ++id) {
        boot_tasks[id] = xTaskCreateStaticAffinitySet(boot_task, "boot-control", 1024, (void *)(uintptr_t)id,
            2, boot_stack[id], &boot_state[id], 255);
        configASSERT(boot_tasks[id]);
    }
    vTaskCoreAffinitySet(boot_tasks[0], 1U << 6);
    vTaskPrioritySet(boot_tasks[0], 4);
    configASSERT(uxTaskPriorityGet(boot_tasks[0]) == 4 && vTaskCoreAffinityGet(boot_tasks[0]) == (1U << 6));
    vTaskCoreAffinitySet(boot_tasks[1], 1U << 5);
    vTaskSuspend(boot_tasks[1]);
    vTaskCoreAffinitySet(boot_tasks[2], 1U << 7);
    vTaskSuspend(boot_tasks[2]);
    vTaskResume(boot_tasks[2]);
    vTaskDelete(boot_tasks[3]);
    vTaskCoreAffinitySet(boot_tasks[4], 1U << 0);
    vTaskPrioritySet(boot_tasks[4], 4);
    vTaskStartScheduler();
    platform_panic("scheduler returned", 0);
}

#endif
