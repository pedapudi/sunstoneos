#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "platform.h"
#include "freertos_control.h"
#include "perturb.h"
#include <stdbool.h>
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
/* The fixture's own timing limits (busy-wait bounds and the local-phase tick
 * bound) scale with an injected scheduler delay, so a delay alone cannot trip
 * them; a lost wake still exceeds any scaled limit. One loaded turn costs a
 * few instructions; a scheduler call costs hundreds. */
static unsigned limit(unsigned base)
{
#ifdef SS_KERNEL_TEST_PERTURB
    const volatile uint32_t *table = (const volatile uint32_t *)(uintptr_t)SS_KERNEL_TEST_PERTURB;
    unsigned turns = 0;
    for (unsigned site = 0; site < SS_PERTURB_SITES; ++site) turns += table[1 + site];
    return base * (1 + turns / 100);
#else
    return base;
#endif
}
static uint64_t local_phase_locks, local_wakes, local_completed, local_ticks, peer_idle_samples;
static unsigned freeze_test, empty_sgis[8], abort_phase, abort_parked, abort_waits;
static unsigned freeze_control_path;
static TaskHandle_t abort_target;
static unsigned migration_phase, migration_started, migration_incoming, migration_adopted;
static TaskHandle_t migration_target;
static unsigned deferred_peer_started, deferred_peer_ran;
static unsigned remote_suspend_returned;
static unsigned admission_started, admission_go, admission_armed, admission_waiting, admission_completed;
static unsigned admission_phase[9], admission_core[9], admission_migrate;
static TaskHandle_t admission_target;
static TaskHandle_t shield_self_target, held_self_target, delete_self_target, local_wake_target;
static unsigned shield_self_phase, shield_premask, shield_fulfilled, shield_remote_frame;
static unsigned held_self_started, held_self_release, held_self_returned, held_self_armed;
static unsigned delete_self_mode, delete_self_go, delete_self_started, delete_self_waiting, delete_self_frozen;
static unsigned local_wake_mode, local_wake_started, local_wake_ran;
extern void ss_kernel_test_freeze_mark(unsigned);
extern void ss_kernel_test_freeze_release(TaskHandle_t);
extern void ss_kernel_test_request(TaskHandle_t, unsigned, unsigned);
extern unsigned ss_kernel_test_task_state(TaskHandle_t);
extern void ss_kernel_test_cleanup_shield(TaskHandle_t, bool);
extern void ss_kernel_test_gate_hold(TaskHandle_t);
extern bool ss_kernel_test_gate_complete(void);
extern void ss_kernel_test_release_after_wait(unsigned);
extern bool ss_kernel_test_redirect_saved(unsigned);
extern bool ss_kernel_test_route_returned(TaskHandle_t);
extern bool ss_latency_task_suspended(void *);
extern void ss_kernel_test_yield_in_object(const void *);
extern void ss_kernel_test_pending_slice(void);
extern bool ss_kernel_test_current_running(void);
extern bool ss_kernel_test_delay_aborted(void);
extern unsigned ss_kernel_test_suspension(TaskHandle_t);
extern bool ss_kernel_test_frame_pending(TaskHandle_t);
extern unsigned ss_kernel_test_drain_held(TaskHandle_t);
extern bool ss_kernel_test_freeze_unacked(unsigned);
extern void ss_kernel_test_object_admission(const void *, unsigned);
extern void ss_kernel_test_resume_then_suspend(TaskHandle_t, bool);
extern void ssKernelObjectSuspendOn(const void *);
extern BaseType_t ssKernelObjectResumeOn(void);
void ss_kernel_test_gate_acquiring(void)
{
    if (__atomic_load_n(&held_self_armed, __ATOMIC_ACQUIRE))
        __atomic_store_n(&held_self_release, 1, __ATOMIC_RELEASE);
    unsigned kind = __atomic_load_n(&admission_armed, __ATOMIC_ACQUIRE);
    if (!kind) return;
    __atomic_store_n(&admission_go, kind, __ATOMIC_RELEASE);
    for (unsigned wait = 0; wait < limit(1000000) && __atomic_load_n(&admission_waiting, __ATOMIC_ACQUIRE) != kind; ++wait)
        __asm__ volatile("nop");
    configASSERT(admission_waiting == kind);
}
void ss_kernel_test_admission_wait(unsigned cpu, unsigned phase)
{
    (void)cpu;
    if (xTaskGetCurrentTaskHandle() == delete_self_target) {
        __atomic_store_n(&delete_self_waiting, 1, __ATOMIC_RELEASE);
        return;
    }
    if (xTaskGetCurrentTaskHandle() != admission_target) return;
    UBaseType_t irq = portSET_INTERRUPT_MASK();
    configASSERT(irq > portMAX_API_PRIORITY_MASK);
    portCLEAR_INTERRUPT_MASK(irq);
    unsigned kind = __atomic_load_n(&admission_go, __ATOMIC_ACQUIRE);
    if (__atomic_load_n(&admission_waiting, __ATOMIC_ACQUIRE) != kind) admission_phase[kind] = phase;
    __atomic_store_n(&admission_waiting, kind, __ATOMIC_RELEASE);
}
void ss_kernel_test_admission_resume(void)
{
    if (xTaskGetCurrentTaskHandle() == admission_target && __atomic_load_n(&admission_migrate, __ATOMIC_ACQUIRE))
        taskYIELD();
}
void ss_kernel_test_selecting(unsigned cpu, bool mandatory)
{
    unsigned freeze = __atomic_load_n(&freeze_test, __ATOMIC_ACQUIRE);
    if ((cpu == 0 && (freeze == 2 || freeze == 5 || freeze == 8)) ||
        (cpu == 1 && __atomic_load_n(&held_self_armed, __ATOMIC_ACQUIRE)))
        configASSERT(mandatory && ss_kernel_test_freeze_unacked(cpu));
}
void ss_kernel_test_self_suspend_published(void)
{
    if (xTaskGetCurrentTaskHandle() == shield_self_target && shield_premask) {
        shield_premask = 0;
        portYIELD();
        shield_fulfilled = 1;
    }
}
void ss_kernel_test_self_delete_mark(unsigned cpu)
{
    if (xTaskGetCurrentTaskHandle() == delete_self_target && delete_self_mode == 2) {
        ss_kernel_test_freeze_mark(cpu);
        __atomic_store_n(&delete_self_frozen, 1, __ATOMIC_RELEASE);
    }
}
void ss_kernel_test_remote_suspend_mark(unsigned cpu)
{
    if (xTaskGetCurrentTaskHandle() == shield_self_target && shield_self_phase == 13) {
        configASSERT(ss_kernel_test_suspension(shield_self_target) & SS_CONTROL_OFFCPU);
        configASSERT(!ss_latency_task_suspended(shield_self_target));
        shield_remote_frame = 1;
    }
    unsigned freeze = __atomic_load_n(&freeze_test, __ATOMIC_ACQUIRE);
    if (cpu == 0 && (freeze == 1 || freeze == 4) && xTaskGetCurrentTaskHandle() == controller) {
        /* At step 1 the controller suspends itself. An interrupt exit between
         * the suspend's publication and its local transition can complete the
         * suspension here instead (see ssKernelSuspendCurrent); the freeze
         * must then be checked on this path. */
        if (freeze == 1) freeze_control_path = 1;
        ss_kernel_test_freeze_mark(cpu);
        __atomic_store_n(&freeze_test, freeze + 1, __ATOMIC_RELEASE);
    }
}
void ss_kernel_test_detach_incoming(unsigned slot, bool queued)
{
    (void)slot;
    if (portGET_CORE_ID() == 2 && __atomic_load_n(&migration_phase, __ATOMIC_ACQUIRE) == 1) {
        configASSERT(!queued);
        ++migration_incoming;
    }
}
void ss_kernel_test_ineligible_adopt(unsigned cpu, bool queued)
{
    if (__atomic_load_n(&migration_phase, __ATOMIC_ACQUIRE) < 3) {
        configASSERT(!queued && cpu == 3);
        ++migration_adopted;
    }
}
void ss_kernel_test_saved_frame(unsigned cpu, unsigned slot)
{
    if (cpu == 0 && xTaskGetCurrentTaskHandle() == shield_self_target && shield_self_phase == 10) {
        shield_self_phase = 11;
        ss_kernel_test_resume_then_suspend(shield_self_target, true);
    }
    if (cpu == 2 && __atomic_load_n(&migration_phase, __ATOMIC_ACQUIRE) == 1 &&
        xTaskGetCurrentTaskHandle() == migration_target && ss_kernel_test_redirect_saved(slot))
        __atomic_store_n(&migration_phase, 2, __ATOMIC_RELEASE);
}
void ss_kernel_test_sgi(void)
{ __atomic_fetch_add(&empty_sgis[portGET_CORE_ID()], 1, __ATOMIC_RELEASE); }
void ss_kernel_test_direct_suspend_mark(unsigned cpu)
{
    if (xTaskGetCurrentTaskHandle() == shield_self_target && shield_fulfilled)
        configASSERT(shield_self_phase != 6);
    if (cpu == 0 && __atomic_load_n(&freeze_test, __ATOMIC_ACQUIRE) == 1 &&
        xTaskGetCurrentTaskHandle() == controller) {
        ss_kernel_test_freeze_mark(cpu);
        __atomic_store_n(&freeze_test, 2, __ATOMIC_RELEASE);
    }
}
void ss_kernel_test_after_control_batch(unsigned cpu)
{
    if (cpu == 0 && __atomic_load_n(&abort_phase, __ATOMIC_ACQUIRE) == 1 &&
        ss_kernel_test_gate_complete()) {
        __atomic_store_n(&abort_parked, 1, __ATOMIC_RELEASE);
    }
}
void ss_kernel_test_wait_blocked(unsigned slot, uint64_t generation)
{
    (void)generation;
    if (xTaskGetCurrentTaskHandle() == controller &&
        __atomic_load_n(&freeze_test, __ATOMIC_ACQUIRE) == 7) {
        ss_kernel_test_freeze_mark(0);
        __atomic_store_n(&freeze_test, 8, __ATOMIC_RELEASE);
    }
    if (xTaskGetCurrentTaskHandle() == abort_target) {
        if (++abort_waits == 2) ss_kernel_test_release_after_wait(slot);
    }
}
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
    /* TPIDRRO still identifies the saved outgoing frame until port restore. */
    unsigned idle = uxTaskPriorityGet(pxCurrentTCBs[cpu]) == 0;
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
        /* The first wait spans the controller's setup checks, whose length
         * grows with an injected delay; the controller's reply timeout bounds
         * the first round instead. */
        uint32_t notified = ulTaskNotifyTake(pdTRUE,
            worker_round[id] == ROUNDS || worker_round[id] == 0 ? portMAX_DELAY : pdMS_TO_TICKS(2000));
        if (notified != 1) {
            platform_puts("worker wait cpu="); platform_hex(id);
            platform_puts(" control-round="); platform_hex(control_round);
            platform_puts(" control-phase="); platform_hex(control_phase); platform_puts("\n");
            extern void ss_kernel_test_dump_control(TaskHandle_t);
            ss_kernel_test_dump_control(controller);
            ss_kernel_test_dump_control(movers[0]);
            ss_kernel_test_dump_control(movers[1]);
        }
        configASSERT(notified == 1);
        configASSERT(worker_round[id] < ROUNDS);
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
        unsigned freeze = __atomic_load_n(&freeze_test, __ATOMIC_ACQUIRE);
        if (id == 0 && (freeze == 2 || freeze == 5 || freeze == 8)) {
            ss_kernel_test_freeze_release(controller);
            __atomic_store_n(&freeze_test, freeze == 2 ? 3 : freeze == 5 ? 6 : 9, __ATOMIC_RELEASE);
            if (freeze == 8) configASSERT(xTaskAbortDelay(controller) == pdTRUE);
            else vTaskResume(controller);
        }
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
static unsigned boot_suspend_checks[BOOT_TASKS];
void ss_kernel_test_before_self_suspend(uint64_t *repair, uint64_t bit)
{
    TaskHandle_t current = xTaskGetCurrentTaskHandle();
    for (unsigned id = 0; id < BOOT_TASKS; ++id)
        if (boot_tasks[id] == current) {
            /* Retain a prior repair indication across the new publication.
             * Completing current-task metadata must retire this indication. */
            *repair |= bit;
            return;
        }
}
void ss_kernel_test_self_suspend(unsigned consumed, bool repair, bool suspended,
                                  bool off_cpu, bool acknowledged)
{
    TaskHandle_t current = xTaskGetCurrentTaskHandle();
    for (unsigned id = 0; id < BOOT_TASKS; ++id) {
        if (boot_tasks[id] != current) continue;
        /* The direct transition consumes no metadata record. A prior repair
         * stays pending, and only the saved frame can acknowledge suspension. */
        configASSERT(consumed == 0 && repair && suspended && !off_cpu && !acknowledged);
        configASSERT(!ss_latency_task_suspended(current));
        __atomic_fetch_add(&boot_suspend_checks[id], 1, __ATOMIC_RELEASE);
        return;
    }
}
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
    for (unsigned id = 0; id < BOOT_TASKS; ++id) {
        if (id == 3) continue;
        for (unsigned wait = 0; wait < 200 && !__atomic_load_n(&boot_suspend_checks[id], __ATOMIC_ACQUIRE); ++wait)
            vTaskDelay(1);
        /* Unperturbed, the resumed task suspends itself through the direct
         * transition, which the hook checks once. An injected delay can let the
         * resume's control record still be open then, and the suspension
         * completes through the control path without the hook; the task must
         * still end suspended (checked below). */
        unsigned checks = __atomic_load_n(&boot_suspend_checks[id], __ATOMIC_ACQUIRE);
        configASSERT(checks == 1 || (limit(1) > 1 && checks == 0));
        for (unsigned wait = 0; wait < 200 && !ss_latency_task_suspended(boot_tasks[id]); ++wait) vTaskDelay(1);
        configASSERT(ss_latency_task_suspended(boot_tasks[id]));
    }
}
static StaticTask_t control_race_state[11];
static StackType_t control_race_stack[11][2048];
static StaticQueue_t shield_queue_state;
static uint8_t shield_queue_storage[sizeof(uint32_t)];
static QueueHandle_t shield_queue;
static StaticQueue_t local_wake_queue_state;
static uint8_t local_wake_queue_storage[sizeof(uint32_t)];
static QueueHandle_t local_wake_queue;
static void abort_actor(void *unused)
{
    (void)unused;
    configASSERT(ulTaskNotifyTake(pdTRUE, portMAX_DELAY) == 1);
    configASSERT(abort_parked && abort_phase == 1);
    __atomic_store_n(&abort_phase, 2, __ATOMIC_RELEASE);
    configASSERT(ulTaskNotifyTake(pdTRUE, portMAX_DELAY) == 1);
    __atomic_store_n(&abort_phase, 3, __ATOMIC_RELEASE);
    for (;;) vTaskSuspend(NULL);
}
static void shield_actor(void *unused)
{
    (void)unused;
    uint32_t value;
    (void)xQueueReceive(shield_queue, &value, portMAX_DELAY);
    platform_panic("shielded event wait unexpectedly completed", 0);
}
static void migration_actor(void *unused)
{
    (void)unused;
    __atomic_store_n(&migration_started, 1, __ATOMIC_RELEASE);
    while (__atomic_load_n(&migration_phase, __ATOMIC_ACQUIRE) < 2) __asm__ volatile("nop");
    configASSERT(portGET_CORE_ID() == 2 && ss_kernel_test_route_returned(xTaskGetCurrentTaskHandle()));
    __atomic_store_n(&migration_phase, 3, __ATOMIC_RELEASE);
    for (;;) vTaskSuspend(NULL);
}
static void deferred_peer(void *unused)
{
    (void)unused;
    __atomic_store_n(&deferred_peer_started, 1, __ATOMIC_RELEASE);
    configASSERT(ulTaskNotifyTake(pdTRUE, portMAX_DELAY) == 1);
    __atomic_store_n(&deferred_peer_ran, 1, __ATOMIC_RELEASE);
    for (;;) vTaskSuspend(NULL);
}
static void remote_suspend_actor(void *unused)
{
    (void)unused;
    configASSERT(ulTaskNotifyTake(pdTRUE, portMAX_DELAY) == 1);
    vTaskSuspend(controller);
    __atomic_store_n(&remote_suspend_returned, 1, __ATOMIC_RELEASE);
    for (;;) vTaskSuspend(NULL);
}
static void admission_actor(void *unused)
{
    (void)unused;
    __atomic_store_n(&admission_started, 1, __ATOMIC_RELEASE);
    for (unsigned kind = 1; kind <= 8; ++kind) {
        while (__atomic_load_n(&admission_go, __ATOMIC_ACQUIRE) != kind) __asm__ volatile("nop");
        if (kind == 1) vTaskDelay(1);
        else if (kind == 2) configASSERT(ulTaskNotifyTake(pdTRUE, portMAX_DELAY) == 1);
        else if (kind <= 6) ss_kernel_test_object_admission(empty, kind - 3);
        else if (kind == 7) configASSERT(xTaskGetSchedulerState() == taskSCHEDULER_RUNNING);
        else {
            UBaseType_t irq = portSET_INTERRUPT_MASK();
            ss_kernel_test_object_admission(empty, 0);
            portCLEAR_INTERRUPT_MASK(irq);
        }
        admission_core[kind] = portGET_CORE_ID();
        __atomic_store_n(&admission_completed, kind, __ATOMIC_RELEASE);
    }
    for (;;) vTaskSuspend(NULL);
}
static void admission_verify(void)
{
    admission_target = xTaskCreateStaticAffinitySet(admission_actor, "freeze-admission", 2048,
        NULL, 6, control_race_stack[5], &control_race_state[5], 2);
    configASSERT(admission_target);
    for (unsigned wait = 0; wait < 200 && !__atomic_load_n(&admission_started, __ATOMIC_ACQUIRE); ++wait) vTaskDelay(1);
    configASSERT(admission_started);
    for (unsigned kind = 1; kind <= 8; ++kind) {
        __atomic_store_n(&admission_armed, kind & 1u ? kind : 0, __ATOMIC_RELEASE);
        vTaskSuspendAll();
        /* The holder may consume an available notification with a nonzero
         * timeout. Only an actual attempt to block is forbidden here. */
        xTaskNotifyGive(controller);
        configASSERT(ulTaskNotifyTake(pdTRUE, portMAX_DELAY) == 1);
        __atomic_store_n(&admission_go, kind, __ATOMIC_RELEASE);
        for (unsigned wait = 0; wait < limit(1000000) && (kind == 8 || __atomic_load_n(&admission_waiting, __ATOMIC_ACQUIRE) != kind) &&
             __atomic_load_n(&admission_completed, __ATOMIC_ACQUIRE) < kind; ++wait) __asm__ volatile("nop");
        if (kind == 8) configASSERT(admission_completed == 8 && admission_waiting != 8);
        else {
            configASSERT(admission_waiting == kind && admission_completed < kind);
            configASSERT(admission_phase[kind] == (kind & 1u ? 1u : 2u));
        }
        if (kind == 2) {
            __atomic_store_n(&admission_migrate, 1, __ATOMIC_RELEASE);
            vTaskCoreAffinitySet(admission_target, 4);
        }
        (void)xTaskResumeAll();
        __atomic_store_n(&admission_migrate, 0, __ATOMIC_RELEASE);
        if (kind == 2) xTaskNotifyGive(admission_target);
        for (unsigned wait = 0; wait < 200 && __atomic_load_n(&admission_completed, __ATOMIC_ACQUIRE) < kind; ++wait) vTaskDelay(1);
        configASSERT(admission_completed == kind && admission_core[kind] == (kind == 1 ? 1u : 2u));
        __atomic_store_n(&admission_armed, 0, __ATOMIC_RELEASE);
    }
    vTaskDelete(admission_target);
    admission_target = NULL;
}
static void local_wake_actor(void *unused)
{
    (void)unused;
    __atomic_store_n(&local_wake_started, 1, __ATOMIC_RELEASE);
    if (!local_wake_mode) {
        uint32_t value;
        configASSERT(xQueueReceive(local_wake_queue, &value, portMAX_DELAY) == pdPASS && value == 0x42);
    } else configASSERT(ulTaskNotifyTake(pdTRUE, portMAX_DELAY) == 1);
    __atomic_store_n(&local_wake_ran, 1, __ATOMIC_RELEASE);
    for (;;) vTaskSuspend(NULL);
}
static void local_wakes_verify(void)
{
    local_wake_queue = xQueueCreateStatic(1, sizeof(uint32_t), local_wake_queue_storage, &local_wake_queue_state);
    configASSERT(local_wake_queue);
    for (unsigned mode = 0; mode < 2; ++mode) {
        local_wake_mode = mode;
        local_wake_started = local_wake_ran = 0;
        local_wake_target = xTaskCreateStaticAffinitySet(local_wake_actor, "local-control-wake", 2048,
            NULL, 6, control_race_stack[6], &control_race_state[6], 1);
        configASSERT(local_wake_target && local_wake_started && (ss_kernel_test_task_state(local_wake_target) & 1));
        if (!mode) {
            vTaskPreemptionDisable(NULL);
            /* Any interrupt exit after the wake may make the selection that
             * keeps this task, so count selections from before the wake. */
            uint64_t selections = __atomic_load_n(&switches[0], __ATOMIC_ACQUIRE);
            uint32_t value = 0x42;
            configASSERT(xQueueSend(local_wake_queue, &value, 0) == pdPASS);
            configASSERT(local_wake_ran == 0);
            unsigned seen = __atomic_load_n(&empty_sgis[0], __ATOMIC_ACQUIRE);
            portYIELD_CORE(0);
            for (unsigned wait = 0; wait < limit(100000) && __atomic_load_n(&empty_sgis[0], __ATOMIC_ACQUIRE) == seen; ++wait)
                __asm__ volatile("nop");
            configASSERT(__atomic_load_n(&empty_sgis[0], __ATOMIC_ACQUIRE) > seen);
            configASSERT(__atomic_load_n(&switches[0], __ATOMIC_ACQUIRE) > selections);
            configASSERT(local_wake_ran == 0 && (ss_kernel_test_task_state(local_wake_target) & 256));
            vTaskPreemptionEnable(NULL);
            configASSERT(local_wake_ran == 1);
        } else {
            vTaskSuspendAll();
            xTaskNotifyGive(local_wake_target);
            configASSERT((ss_kernel_test_task_state(local_wake_target) & 257) == 1);
            (void)xTaskResumeAll();
            configASSERT(local_wake_ran == 1);
        }
        vTaskDelete(local_wake_target);
    }
}
static void held_self_actor(void *unused)
{
    (void)unused;
    vTaskPreemptionDisable(NULL);
    ssKernelObjectSuspendOn(empty);
    unsigned seen = __atomic_load_n(&empty_sgis[1], __ATOMIC_ACQUIRE);
    vTaskSuspend(NULL);
    for (unsigned wait = 0; wait < limit(100000) && __atomic_load_n(&empty_sgis[1], __ATOMIC_ACQUIRE) == seen; ++wait)
        __asm__ volatile("nop");
    configASSERT(__atomic_load_n(&empty_sgis[1], __ATOMIC_ACQUIRE) > seen);
    __atomic_store_n(&held_self_started, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&held_self_release, __ATOMIC_ACQUIRE)) __asm__ volatile("nop");
    (void)ssKernelObjectResumeOn();
    __atomic_store_n(&held_self_returned, 1, __ATOMIC_RELEASE);
    vTaskPreemptionEnable(NULL);
    for (;;) vTaskSuspend(NULL);
}
static void held_self_verify(void)
{
    held_self_target = xTaskCreateStaticAffinitySet(held_self_actor, "held-self", 2048,
        NULL, 6, control_race_stack[7], &control_race_state[7], 2);
    configASSERT(held_self_target);
    for (unsigned wait = 0; wait < 200 && !__atomic_load_n(&held_self_started, __ATOMIC_ACQUIRE); ++wait) vTaskDelay(1);
    configASSERT(held_self_started);
    __atomic_store_n(&held_self_armed, 1, __ATOMIC_RELEASE);
    vTaskSuspendAll();
    configASSERT(!held_self_returned && xTaskGetCurrentTaskHandleForCore(1) != held_self_target);
    __atomic_store_n(&held_self_armed, 0, __ATOMIC_RELEASE);
    (void)xTaskResumeAll();
    for (unsigned wait = 0; wait < 200 && !ss_latency_task_suspended(held_self_target); ++wait) vTaskDelay(1);
    configASSERT(ss_latency_task_suspended(held_self_target));
    vTaskResume(held_self_target);
    for (unsigned wait = 0; wait < 200 && !__atomic_load_n(&held_self_returned, __ATOMIC_ACQUIRE); ++wait) vTaskDelay(1);
    configASSERT(held_self_returned);
    vTaskDelete(held_self_target);
}
static void shield_self_actor(void *unused)
{
    (void)unused;
    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    shield_self_target = self;
    ss_kernel_test_cleanup_shield(self, true);
    shield_self_phase = 10;
    vTaskSuspend(NULL);
    configASSERT(shield_self_phase == 11);
    configASSERT((ss_kernel_test_suspension(self) &
        (SS_CONTROL_SUSPENDED | SS_CONTROL_OFFCPU | SS_CONTROL_SELF | SS_CONTROL_SELF_FRAME | SS_CONTROL_REMOTE_FRAME)) ==
        (SS_CONTROL_OFFCPU | SS_CONTROL_REMOTE_FRAME));
    configASSERT(ss_kernel_test_frame_pending(self));
    shield_self_phase = 12;
    configASSERT(ulTaskNotifyTake(pdTRUE, portMAX_DELAY) == 1);
    configASSERT(shield_self_phase == 13);
    ss_kernel_test_cleanup_shield(self, false);
    while (ss_kernel_test_suspension(self) & SS_CONTROL_REMOTE_FRAME) taskYIELD();
    configASSERT(shield_remote_frame && !ss_kernel_test_frame_pending(self));
    shield_self_phase = 14;
    ss_kernel_test_cleanup_shield(self, true);
    shield_self_phase = 1;
    vTaskSuspend(NULL);
    configASSERT(shield_self_phase == 2);
    shield_self_phase = 3;
    vTaskSuspend(NULL);
    configASSERT(shield_self_phase == 4);
    shield_self_phase = 5;
    shield_premask = 1;
    vTaskSuspend(NULL);
    configASSERT(shield_self_phase == 6);
    shield_self_phase = 7;
    vTaskSuspend(NULL);
    configASSERT(shield_self_phase == 8);
    shield_self_phase = 9;
    ss_kernel_test_cleanup_shield(self, false);
    for (;;) vTaskSuspend(NULL);
}
static void shield_self_wait(unsigned phase)
{
    for (unsigned wait = 0; wait < 200 &&
         (shield_self_phase != phase || !ss_latency_task_suspended(shield_self_target)); ++wait) vTaskDelay(1);
    configASSERT(shield_self_phase == phase && ss_latency_task_suspended(shield_self_target));
}
static void shield_self_verify(void)
{
    shield_self_target = xTaskCreateStaticAffinitySet(shield_self_actor, "shield-self", 2048,
        NULL, 6, control_race_stack[8], &control_race_state[8], 1);
    configASSERT(shield_self_target);
    for (unsigned wait = 0; wait < 200 && shield_self_phase != 12; ++wait) vTaskDelay(1);
    configASSERT(shield_self_phase == 12);
    shield_self_phase = 13;
    xTaskNotifyGive(shield_self_target);
    for (unsigned wait = 0; wait < 200 && shield_self_phase != 1; ++wait) vTaskDelay(1);
    shield_self_wait(1);
    vTaskPrioritySet(shield_self_target, 7);
    configASSERT(shield_self_phase == 1 && ss_latency_task_suspended(shield_self_target));
    shield_self_phase = 2;
    ss_kernel_test_resume_then_suspend(shield_self_target, false);
    shield_self_wait(3);
    shield_self_phase = 4;
    vTaskResume(shield_self_target);
    shield_self_wait(5);
    shield_self_phase = 6;
    ss_kernel_test_resume_then_suspend(shield_self_target, false);
    shield_self_wait(7);
    ss_kernel_test_request(shield_self_target, 4, 0);
    for (unsigned wait = 0; wait < 200 && !(ss_kernel_test_task_state(shield_self_target) & 8); ++wait) vTaskDelay(1);
    configASSERT((ss_kernel_test_task_state(shield_self_target) & 40) == 40);
    shield_self_phase = 8;
    vTaskResume(shield_self_target);
    configASSERT(shield_self_phase == 9 || !(ss_kernel_test_suspension(shield_self_target) & SS_CONTROL_SELF));
    for (unsigned wait = 0; wait < 200 && shield_self_phase != 9; ++wait) vTaskDelay(1);
    configASSERT(shield_self_phase == 9);
    for (unsigned wait = 0; wait < 200 && eTaskGetState(shield_self_target) != eDeleted; ++wait) vTaskDelay(1);
    configASSERT(eTaskGetState(shield_self_target) == eDeleted);
    shield_self_target = NULL;
}
static void delete_self_actor(void *unused)
{
    (void)unused;
    __atomic_store_n(&delete_self_started, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&delete_self_go, __ATOMIC_ACQUIRE)) __asm__ volatile("nop");
    vTaskDelete(NULL);
    platform_panic("self deletion returned", 0);
}
static void delete_self_verify(void)
{
    for (unsigned mode = 1; mode <= 2; ++mode) {
        delete_self_mode = mode;
        delete_self_started = delete_self_go = delete_self_waiting = delete_self_frozen = 0;
        delete_self_target = xTaskCreateStaticAffinitySet(delete_self_actor, "delete-self-gate", 2048,
            NULL, 6, control_race_stack[8 + mode], &control_race_state[8 + mode], 4);
        configASSERT(delete_self_target);
        for (unsigned wait = 0; wait < 200 && !__atomic_load_n(&delete_self_started, __ATOMIC_ACQUIRE); ++wait) vTaskDelay(1);
        configASSERT(delete_self_started);
        if (mode == 1) vTaskSuspendAll();
        __atomic_store_n(&delete_self_go, 1, __ATOMIC_RELEASE);
        if (mode == 1) {
            for (unsigned wait = 0; wait < limit(1000000) && !__atomic_load_n(&delete_self_waiting, __ATOMIC_ACQUIRE); ++wait)
                __asm__ volatile("nop");
            configASSERT(delete_self_waiting && eTaskGetState(delete_self_target) == eRunning);
            (void)xTaskResumeAll();
        } else {
            for (unsigned wait = 0; wait < limit(1000000) && (!__atomic_load_n(&delete_self_frozen, __ATOMIC_ACQUIRE) ||
                 ss_kernel_test_freeze_unacked(2)); ++wait) __asm__ volatile("nop");
            configASSERT(delete_self_frozen && xTaskGetCurrentTaskHandleForCore(2) != delete_self_target);
            ss_kernel_test_freeze_release(delete_self_target);
        }
        for (unsigned wait = 0; wait < 200 && eTaskGetState(delete_self_target) != eDeleted; ++wait) vTaskDelay(1);
        configASSERT(eTaskGetState(delete_self_target) == eDeleted);
        delete_self_target = NULL;
    }
}
static void control_races_verify(void)
{
    /* The freeze steps hold CPU0's gate until CPU0's background task releases it. A mover may start on any core at a
     * priority above the background; one that enters the kernel on CPU0 during a freeze waits at admission and starves
     * the release. Each mover blocks on its first notification before any freeze. */
    for (unsigned id = 0; id < 2; ++id) {
        for (unsigned wait = 0; wait < 200 && eTaskGetState(movers[id]) != eBlocked; ++wait) vTaskDelay(1);
        configASSERT(eTaskGetState(movers[id]) == eBlocked);
    }
    /* The peer outranks the controller. At equal priority the voluntary yield
     * can select it just before a tick, and the slice ending at that tick
     * returns the core to the controller before the peer leaves its wait. */
    TaskHandle_t peer = xTaskCreateStaticAffinitySet(deferred_peer, "deferred-yield", 2048,
        NULL, 6, control_race_stack[3], &control_race_state[3], 1);
    configASSERT(peer);
    for (unsigned wait = 0; wait < 200 && !__atomic_load_n(&deferred_peer_started, __ATOMIC_ACQUIRE); ++wait) vTaskDelay(1);
    configASSERT(deferred_peer_started);
    vTaskPreemptionDisable(NULL);
    xTaskNotifyGive(peer);
    configASSERT(!deferred_peer_ran);
    unsigned slice_sgi = __atomic_load_n(&empty_sgis[0], __ATOMIC_ACQUIRE);
    ss_kernel_test_pending_slice();
    for (unsigned wait = 0; wait < limit(100000) && __atomic_load_n(&empty_sgis[0], __ATOMIC_ACQUIRE) == slice_sgi; ++wait)
        __asm__ volatile("nop");
    configASSERT(__atomic_load_n(&empty_sgis[0], __ATOMIC_ACQUIRE) > slice_sgi);
    configASSERT(deferred_peer_ran == 0);
    ss_kernel_test_yield_in_object(empty);
    configASSERT(deferred_peer_ran == 1);
    vTaskPreemptionEnable(NULL);
    vTaskDelete(peer);
    /* A maintenance SGI with no published work cannot force C selection. */
    vTaskDelay(4);
    uint64_t before = __atomic_load_n(&switches[0], __ATOMIC_ACQUIRE);
    for (unsigned n = 0; n < 8; ++n) {
        unsigned seen = __atomic_load_n(&empty_sgis[0], __ATOMIC_ACQUIRE);
        portYIELD_CORE(0);
        for (unsigned wait = 0; wait < limit(100000) && __atomic_load_n(&empty_sgis[0], __ATOMIC_ACQUIRE) == seen; ++wait)
            __asm__ volatile("nop");
        configASSERT(__atomic_load_n(&empty_sgis[0], __ATOMIC_ACQUIRE) > seen);
        configASSERT(__atomic_load_n(&switches[0], __ATOMIC_ACQUIRE) == before);
    }
    __atomic_store_n(&freeze_test, 1, __ATOMIC_RELEASE);
    vTaskSuspend(NULL);
    configASSERT(__atomic_load_n(&freeze_test, __ATOMIC_ACQUIRE) == 3);
#ifndef SS_STORM
    /* Without the interrupt storm and an injected delay, the step-1 freeze
     * must be checked on the direct local transition. */
    configASSERT(limit(1) > 1 || !freeze_control_path);
#endif
    TaskHandle_t remote = xTaskCreateStaticAffinitySet(remote_suspend_actor, "freeze-remote", 2048,
        NULL, 6, control_race_stack[4], &control_race_state[4], 2);
    configASSERT(remote);
    __atomic_store_n(&freeze_test, 4, __ATOMIC_RELEASE);
    xTaskNotifyGive(remote);
    for (unsigned wait = 0; wait < limit(10000000) && __atomic_load_n(&freeze_test, __ATOMIC_ACQUIRE) != 6; ++wait) {
        configASSERT(ss_kernel_test_current_running());
    }
    configASSERT(freeze_test == 6);
    for (unsigned wait = 0; wait < 200 && !__atomic_load_n(&remote_suspend_returned, __ATOMIC_ACQUIRE); ++wait) vTaskDelay(1);
    configASSERT(remote_suspend_returned);
    vTaskDelete(remote);
    __atomic_store_n(&freeze_test, 7, __ATOMIC_RELEASE);
    vTaskDelay(100);
    configASSERT(freeze_test == 9);
    configASSERT(!ss_kernel_test_delay_aborted());
    vTaskPrioritySet(boot_tasks[4], ss_kernel_test_drain_held(boot_tasks[4]));
    admission_verify();
    local_wakes_verify();
    held_self_verify();
    shield_self_verify();
    delete_self_verify();
    abort_target = xTaskCreateStaticAffinitySet(abort_actor, "abort-generation", 2048,
        NULL, 6, control_race_stack[0], &control_race_state[0], 1);
    configASSERT(abort_target);
    for (unsigned wait = 0; wait < 200 && !(ss_kernel_test_task_state(abort_target) & 1); ++wait) vTaskDelay(1);
    configASSERT(ss_kernel_test_task_state(abort_target) & 1);
    abort_waits = 1; /* The first admission can run before create returns. */
    ss_kernel_test_gate_hold(abort_target);
    __atomic_store_n(&abort_phase, 1, __ATOMIC_RELEASE);
    ss_kernel_test_request(abort_target, 0, 0);
    for (unsigned wait = 0; wait < 200 && abort_phase != 2; ++wait) vTaskDelay(1);
    configASSERT(abort_parked && abort_phase == 2 && abort_waits == 2);
    /* The accepted abort belongs to the first wait. Its delayed consumption
     * advances the command counter while preserving this later blocked wait. */
    for (unsigned wait = 0; wait < 200 && !(ss_kernel_test_task_state(abort_target) & 128); ++wait) vTaskDelay(1);
    configASSERT((ss_kernel_test_task_state(abort_target) & 129) == 129);
    xTaskNotifyGive(abort_target);
    configASSERT(abort_phase == 3);
    vTaskDelete(abort_target);
    abort_target = NULL;
    shield_queue = xQueueCreateStatic(1, sizeof(uint32_t), shield_queue_storage, &shield_queue_state);
    configASSERT(shield_queue);
    TaskHandle_t shield = xTaskCreateStaticAffinitySet(shield_actor, "shielded-event", 2048,
        NULL, 2, control_race_stack[1], &control_race_state[1], 1);
    configASSERT(shield);
    for (unsigned wait = 0; wait < 200 && !(ss_kernel_test_task_state(shield) & 2); ++wait) vTaskDelay(1);
    configASSERT((ss_kernel_test_task_state(shield) & 3) == 3);
    ss_kernel_test_cleanup_shield(shield, true);
    ss_kernel_test_request(shield, 2, 3);
    ss_kernel_test_request(shield, 1, 0);
    for (unsigned wait = 0; wait < 200 && (ss_kernel_test_task_state(shield) & 79) != 75; ++wait) vTaskDelay(1);
    configASSERT((ss_kernel_test_task_state(shield) & 79) == 75);
    ss_kernel_test_cleanup_shield(shield, false);
    for (unsigned wait = 0; wait < 200 && !(ss_kernel_test_task_state(shield) & 32); ++wait) vTaskDelay(1);
    configASSERT((ss_kernel_test_task_state(shield) & 38) == 32);
    vTaskDelete(shield);
    migration_target = xTaskCreateStaticAffinitySet(migration_actor, "redirect-handoff", 2048,
        NULL, 6, control_race_stack[2], &control_race_state[2], 1u << 2);
    configASSERT(migration_target);
    for (unsigned wait = 0; wait < 200 && !__atomic_load_n(&migration_started, __ATOMIC_ACQUIRE); ++wait) vTaskDelay(1);
    configASSERT(migration_started);
    __atomic_store_n(&migration_phase, 1, __ATOMIC_RELEASE);
    ss_kernel_test_request(migration_target, 3, 1u << 3);
    for (unsigned wait = 0; wait < 200 && __atomic_load_n(&migration_phase, __ATOMIC_ACQUIRE) != 3; ++wait) vTaskDelay(1);
    configASSERT(migration_phase == 3 && migration_incoming && migration_adopted);
    vTaskDelete(migration_target);
    platform_puts("control races: 8 empty SGIs, freeze after local suspension, parked abort preserves next wait, shielded linked-event repair, incoming hint during redirected handoff\n");
}
static void control(void *unused)
{
    (void)unused;
    boot_controls_verify();
    control_races_verify();
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
    for (unsigned id = 2; id < 8; ++id) { control_phase = 0x500 + id; vTaskSuspend(backgrounds[id]); }
    control_phase = 0x510;
    local_receiver = xTaskCreateStaticAffinitySet(local_receive, "local-receiver", 2048,
        NULL, 4, local_stack[0], &local_state[0], 2);
    local_sender = xTaskCreateStaticAffinitySet(local_send, "local-sender", 2048,
        NULL, 2, local_stack[1], &local_state[1], 2);
    configASSERT(local_receiver && local_sender);
    control_phase = 0x511;
    vTaskCoreAffinitySet(local_receiver, 6);
    control_phase = 0x512;
    /* The suspended background tasks leave their cores to the idle tasks asynchronously. */
    for (unsigned wait = 0; wait < 2000 && !__atomic_load_n(&selected_idle[2], __ATOMIC_RELAXED); ++wait) vTaskDelay(1);
    vTaskDelay(4);
    control_phase = 0x513;
    uint64_t busy_start = __atomic_load_n(&busy[1], __ATOMIC_RELAXED);
    xTaskNotifyGive(local_sender);
    control_phase = 0x514;
    /* A liveness bound, not a latency assertion: the 64 local-wake rounds must all complete (the assertions after the print below),
     * and this wait only ends a run that hangs. Under QEMU's round-robin icount the elapsed ticks include the instructions of every
     * core that spins while it waits, so the phase takes 905 ticks in nearly every run, 1,600 in about one run in fifty under host
     * load, and 1,891 with the 3,000-turn owner-drain perturbation of checks/scheduler-control-linked.py (the old 2,000 ms bound failed
     * a gate once). The bound is 4 times the slowest value measured, 1,891 ticks at configTICK_RATE_HZ 1,000: 7,600 ms. Like the
     * phase's tick bound below, it scales with an injected scheduler delay (limit()). */
    configASSERT(ulTaskNotifyTake(pdTRUE, limit(pdMS_TO_TICKS(7600))) == 1);
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
    configASSERT(local_ticks >= ROUNDS && local_ticks < limit(pdMS_TO_TICKS(1000)));
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
