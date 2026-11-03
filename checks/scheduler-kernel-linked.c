/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Included after the generated private tasks translation. The host does not
 * execute task stacks; each checkpoint explicitly saves a frame and enters its
 * owner, so a passing run is linked transition coverage, not an SMP soak. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
static size_t fixture_allocations, fixture_frees, fixture_live;
static long fixture_fail_after = -1;
static bool fixture_heap_protocol;
void *pvPortMalloc(size_t size)
{
    if (fixture_fail_after == 0) return NULL;
    if (fixture_fail_after > 0) --fixture_fail_after;
    void *result = malloc(size);
    if (result) { ++fixture_allocations; ++fixture_live; }
    return result;
}
void vPortFree(void *memory)
{
    if (fixture_heap_protocol) {
        assert(atomic_load(&ssKernelAdminNext) == atomic_load(&ssKernelAdminServing));
        assert(!ssKernelCPUs[ssKernelCPUId()].control_draining);
        assert(!ssKernelCPUs[ssKernelCPUId()].preempt);
        UBaseType_t mask = portSET_INTERRUPT_MASK();
        portCLEAR_INTERRUPT_MASK(mask);
        assert(!mask);
        vTaskSuspendAll(); /* Match heap_4's shared allocator exclusion. */
    }
    if (memory) { assert(fixture_live); --fixture_live; ++fixture_frees; }
    free(memory);
    if (fixture_heap_protocol) (void)xTaskResumeAll();
}
static unsigned fixture_admin;
void ss_kernel_test_admin_enter(unsigned cpu)
{ assert(!ssKernelCPUs[cpu].selecting); ++fixture_admin; }
static unsigned fixture_cpu, fixture_sgi[configNUMBER_OF_CORES], fixture_locks;
unsigned fixture_insert_depth;
void ss_kernel_test_event_insert(unsigned depth) { fixture_insert_depth = depth; }
static unsigned fixture_cleanup, fixture_task_masks, fixture_isr_masks;
static UBaseType_t fixture_api_mask[configNUMBER_OF_CORES];
static void (*fixture_unmask_action)(void);
void ssFixtureCleanup(void *task) { assert(task); ++fixture_cleanup; }
static unsigned fixture_ready_count[SS_OWNER_SLOTS], fixture_ready_cpu[SS_OWNER_SLOTS];
static TaskHandle_t fixture_ready_watch;
static unsigned fixture_ready_baseline;
static bool fixture_ready_payload;
volatile uint64_t ullCriticalNestings[configNUMBER_OF_CORES];
uint64_t ullPortYieldRequired[configNUMBER_OF_CORES];
void platform_panic(const char *file, unsigned line)
{ fprintf(stderr, "%s:%u: kernel assertion\n", file, line); abort(); }
uint64_t ssFixtureTaskRegister(void) { return (uintptr_t)pxCurrentTCBs[fixture_cpu]; }
uint8_t ucPortGetCoreID(void) { return (uint8_t)fixture_cpu; }
uint8_t ucPortGetCoreIDFromIsr(void) { return (uint8_t)fixture_cpu; }
UBaseType_t uxPortSetInterruptMask(void)
{
    ++fixture_task_masks;
    UBaseType_t previous = fixture_api_mask[fixture_cpu];
    fixture_api_mask[fixture_cpu] = 1;
    return previous;
}
UBaseType_t uxPortSetInterruptMaskFromISR(void)
{
    ++fixture_isr_masks;
    UBaseType_t previous = fixture_api_mask[fixture_cpu];
    fixture_api_mask[fixture_cpu] = 1;
    return previous;
}
void vPortClearInterruptMask(UBaseType_t mask)
{
    /* Reopening API IRQs here would permit IRQExit to reenter this owner's
     * partially completed control maintenance, as the real QEMU run did. */
    assert(mask || !ssKernelCPUs[fixture_cpu].control_draining);
    fixture_api_mask[fixture_cpu] = mask;
    if (!mask && fixture_cpu == 0 && fixture_unmask_action && !ssKernelCPUs[fixture_cpu].object_depth &&
        !ssKernelCPUs[fixture_cpu].preempt) {
        void (*action)(void) = fixture_unmask_action;
        fixture_unmask_action = NULL;
        action();
    }
}
void vPortClearInterruptMaskFromISR(UBaseType_t mask)
{ vPortClearInterruptMask(mask); }
void vPortRecursiveLock(uint8_t cpu, ePortRTOSLock lock, BaseType_t acquire)
{
    (void)cpu;
    if (acquire) {
        ++fixture_locks;
        if (lock == eIsrLock)
            for (unsigned slot = 0; slot < SS_OWNER_SLOTS; ++slot)
                assert(atomic_load(&ssKernelControl.slot[slot].tickets) ==
                       atomic_load(&ssKernelControl.slot[slot].serving));
    }
}
void vInterruptCore(uint32_t interrupt, uint8_t cpu)
{
    if (fixture_ready_watch && cpu == 0) {
        unsigned slot = ((TCB_t *)fixture_ready_watch)->uxOwnerSlot;
        assert(fixture_ready_count[slot] == fixture_ready_baseline + 1);
        assert(fixture_ready_payload);
    }
    (void)interrupt; ++fixture_sgi[cpu];
    /* Explicit delivery of gate checkpoints; no concurrent task stacks. */
    if (atomic_load(&ssKernelControl.gate_phase) == SS_GATE_ACQUIRING)
        (void)ss_control_checkpoint(&ssKernelControl, cpu);
}
StackType_t *pxPortInitialiseStack(StackType_t *stack, TaskFunction_t entry, void *arg)
{ (void)entry; (void)arg; *stack = 0; return stack; }
void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{ (void)task; (void)name; abort(); }
static BaseType_t fixture_hook(void *arg) { return *(BaseType_t *)arg; }
static void fixture_entry(void *arg) { (void)arg; }
static StaticTask_t storage[configNUMBER_OF_CORES + 4];
static StackType_t stacks[configNUMBER_OF_CORES + 4][configMINIMAL_STACK_SIZE];
static TaskHandle_t create(unsigned index, unsigned priority)
{
    return xTaskCreateStatic(fixture_entry, "linked", configMINIMAL_STACK_SIZE,
                            NULL, priority, stacks[index], &storage[index]);
}
static struct ssKernelTask *record(TaskHandle_t task)
{ return &ssKernelTasks[((TCB_t *)task)->uxOwnerSlot]; }
static void enter(unsigned cpu)
{
    fixture_cpu = cpu;
    (void)ssKernelOwnerIRQExit();
    unsigned locks = fixture_locks, administration = fixture_admin;
    ssKernelSwitch(cpu);
    assert(fixture_locks == locks && fixture_admin == administration);
    if (!ssKernelCPUs[cpu].preempt) ssKernelCleanup();
}
static void invariant(void)
{
    unsigned running[SS_OWNER_SLOTS] = {0};
    for (unsigned c = 0; c < configNUMBER_OF_CORES; ++c) {
        struct ss_owner *run = &ssKernelCPUs[c].run;
        if (run->current) {
            unsigned slot = run->current->slot;
            assert(++running[slot] == 1);
            assert(atomic_load(&ssKernelTasks[slot].owner) == c);
            assert(!run->current->queued);
        }
        for (unsigned p = 0; p < SS_OWNER_PRIORITIES; ++p)
            for (struct ss_owner_task *t = run->ready[p].head; t; t = t->next) {
                assert(atomic_load(&ssKernelTasks[t->slot].owner) == c);
                assert(t->queued && t->priority == p);
            }
    }
}
#include "scheduler-kernel-objects.c"
void ssFixturePortYield(void)
{ ssKernelOwnerYieldTail(); ssFixtureYield(); }
static void fixture_command_progress(void *opaque)
{
    (void)opaque;
    unsigned caller = fixture_cpu;
    for (unsigned c = 0; c < configNUMBER_OF_CORES; ++c) enter(c);
    fixture_cpu = caller;
}
static void fixture_critical_commands(TaskHandle_t task)
{
    struct ss_owner_backend progress = ssKernelBackend;
    progress.wait = fixture_command_progress;
    ssKernelControl.backend = &progress;
    fixture_cpu = 0;
    unsigned priority = uxTaskPriorityGet(task);
    unsigned isr_masks = fixture_isr_masks;
    uint64_t sequence = atomic_load(&ssKernelControl.slot[((TCB_t *)task)->uxOwnerSlot].applied);
    vTaskEnterCritical();
    vTaskEnterCritical();
    vTaskPrioritySet(task, priority + 1);
    vTaskSuspend(task);
    vTaskResume(task);
    assert(atomic_load(&ssKernelControl.slot[((TCB_t *)task)->uxOwnerSlot].applied) == sequence);
    vTaskExitCritical();
    assert(ssKernelCPUs[0].command_pending[0] || ssKernelCPUs[0].command_pending[1]);
    vTaskExitCritical();
    assert(!ssKernelCPUs[0].command_pending[0] && !ssKernelCPUs[0].command_pending[1]);
    assert(uxTaskPriorityGet(task) == priority + 1);
    assert(fixture_isr_masks == isr_masks);
    /* Leave a deferred self-command for this owner's ordinary service to
     * encounter. Repair's ObjectExitFromISR must not recursively consume it. */
    unsigned owner = atomic_load(&record(task)->owner);
    fixture_cpu = owner;
    ++ssKernelCPUs[owner].preempt;
    vTaskEnterCritical();
    vTaskPrioritySet(task, priority + 2);
    vTaskExitCritical();
    assert(ssKernelCPUs[owner].preempt == 1);
    --ssKernelCPUs[owner].preempt;
    assert(ssKernelCPUs[owner].command_pending[0] || ssKernelCPUs[owner].command_pending[1]);
    ssKernelControlDrain();
    assert(ssKernelCPUs[owner].command_pending[0] || ssKernelCPUs[owner].command_pending[1]);
    ssKernelCommandsFinish(true);
    assert(!ssKernelCPUs[owner].command_pending[0] && !ssKernelCPUs[owner].command_pending[1]);
    fixture_cpu = 0;
    vTaskPrioritySet(task, priority);
    vTaskSuspend(task);
    UBaseType_t irq = vTaskEnterCriticalFromISR();
    isr_masks = fixture_isr_masks;
    assert(!xTaskResumeFromISR(task)); /* Outer exclusion defers the yield. */
    struct ss_control_state desired;
    ss_control_snapshot(&ssKernelControl, ((TCB_t *)task)->uxOwnerSlot, &desired);
    assert(!desired.suspended);
    assert(fixture_isr_masks == isr_masks + 1);
    vTaskExitCriticalFromISR(irq);
    for (unsigned c = 0; c < configNUMBER_OF_CORES; ++c) enter(c);
    assert(uxTaskPriorityGet(task) == priority);
    ssKernelControl.backend = &ssKernelBackend;
    fixture_cpu = 0;
    puts("linked FreeRTOS critical commands: nested task/ISR publication defers owner repair and acknowledgment until compatibility unlock; no FIFO acquires ISR lock");
}
static TaskHandle_t fixture_unlock_caller;
static unsigned fixture_unlock_victim_slot;
static void fixture_unlock_migrate(void)
{
    struct ssKernelTaskCommands *batch = &ssKernelTaskCommands[((TCB_t *)fixture_unlock_caller)->uxOwnerSlot];
    assert(batch->pending[fixture_unlock_victim_slot / 64] &
           (UINT64_C(1) << (fixture_unlock_victim_slot % 64)));
    struct ssKernelMutation mutation = { .command = SS_K_AFFINITY, .argument = 4,
        .slot = ((TCB_t *)fixture_unlock_caller)->uxOwnerSlot };
    (void)ss_control_publish(&ssKernelControl, mutation.slot, ssKernelMutate, &mutation);
    enter(0);
    enter(2);
    assert(pxCurrentTCBs[2] == fixture_unlock_caller);
}
static void fixture_deferred_delete_migration(TaskHandle_t caller)
{
    static StaticTask_t victim_storage;
    static StackType_t victim_stack[configMINIMAL_STACK_SIZE];
    fixture_cpu = 0;
    assert(ssKernelSelf() == caller);
    fixture_api_mask[0] = 0;
    TaskHandle_t victim = xTaskCreateStaticAffinitySet(fixture_entry, "deferred", configMINIMAL_STACK_SIZE,
        NULL, 1, victim_stack, &victim_storage, 2);
    assert(victim);
    fixture_unlock_caller = caller;
    fixture_unlock_victim_slot = ((TCB_t *)victim)->uxOwnerSlot;
    struct ss_owner_backend progress = ssKernelBackend;
    progress.wait = fixture_command_progress;
    ssKernelControl.backend = &progress;
    vTaskEnterCritical();
    vTaskDelete(victim);
    unsigned cleaned = fixture_cleanup;
    /* Interrupt return migrates the caller exactly between outer unlock and
     * task-context acknowledgment processing. Its obligation follows it. */
    fixture_unmask_action = fixture_unlock_migrate;
    vTaskExitCritical();
    assert(fixture_cpu == 2 && fixture_cleanup == cleaned + 1);
    assert(xTaskCreateStaticAffinitySet(fixture_entry, "reuse", configMINIMAL_STACK_SIZE,
        NULL, 1, victim_stack, &victim_storage, 2));
    vTaskDelete((TaskHandle_t)&victim_storage);
    fixture_progress_task = caller;
    progress.wait = fixture_control_progress;
    vTaskCoreAffinitySet(caller, 1);
    assert(fixture_cpu == 0 && ssKernelSelf() == caller);
    ssKernelControl.backend = &ssKernelBackend;
    puts("linked FreeRTOS deferred deletion: caller migration at outer unlock preserves cleanup completion and immediate static buffer reuse");
}
static int fixture_admission(void)
{
    static StaticTask_t tcbs[SS_OWNER_SLOTS + 1];
    static StackType_t buffers[SS_OWNER_SLOTS + 1][configMINIMAL_STACK_SIZE];
    unsigned deleted = fixture_tls_deleted, cleaned = fixture_cleanup;
    assert(!xTaskCreateStaticAffinitySet(fixture_entry, "invalid", configMINIMAL_STACK_SIZE,
        NULL, 1, buffers[SS_OWNER_SLOTS], &tcbs[SS_OWNER_SLOTS], 0));
    assert(fixture_tls_deleted == deleted + 1 && fixture_cleanup == cleaned + 1);
    for (unsigned n = 0; n < SS_OWNER_SLOTS; ++n) {
        TaskHandle_t task = xTaskCreateStatic(fixture_entry, "full", configMINIMAL_STACK_SIZE,
            NULL, n < configNUMBER_OF_CORES ? 0 : 1, buffers[n], &tcbs[n]);
        assert(task);
        if (n < configNUMBER_OF_CORES) {
            xIdleTaskHandles[n] = task;
            pxCurrentTCBs[n] = task;
            ((TCB_t *)task)->uxTaskAttributes |= taskATTRIBUTE_IS_IDLE;
            ((TCB_t *)task)->uxCoreAffinityMask = (UBaseType_t)1 << n;
        }
    }
    assert(uxTaskGetNumberOfTasks() == SS_OWNER_SLOTS);
    assert(!xTaskCreateStatic(fixture_entry, "full", configMINIMAL_STACK_SIZE,
        NULL, 1, buffers[SS_OWNER_SLOTS], &tcbs[SS_OWNER_SLOTS]));
    assert(!xTaskCreateStaticAffinitySet(fixture_entry, "full", configMINIMAL_STACK_SIZE,
        NULL, 1, buffers[SS_OWNER_SLOTS], &tcbs[SS_OWNER_SLOTS], 1));
    assert(fixture_tls_deleted == deleted + 3 && fixture_cleanup == cleaned + 3);
    ssKernelBootstrap();
    xSchedulerRunning = pdTRUE;
    for (unsigned c = 0; c < configNUMBER_OF_CORES; ++c) enter(c);
    struct ss_owner_backend progress = ssKernelBackend;
    progress.wait = fixture_control_progress;
    ssKernelControl.backend = &progress;
    fixture_progress_task = &tcbs[SS_OWNER_SLOTS - 1];
    fixture_cpu = 0;
    vTaskDelete(fixture_progress_task);
    for (unsigned pass = 0; pass < 8; ++pass)
        for (unsigned c = 0; c < configNUMBER_OF_CORES; ++c) enter(c);
    assert(uxTaskGetNumberOfTasks() == SS_OWNER_SLOTS - 1);
    assert(fixture_tls_deleted == deleted + 4 && fixture_cleanup == cleaned + 4);
    fixture_cpu = 0;
    assert(xTaskCreateStaticAffinitySet(fixture_entry, "reuse", configMINIMAL_STACK_SIZE,
        NULL, 1, buffers[SS_OWNER_SLOTS], &tcbs[SS_OWNER_SLOTS], 1));
    assert(uxTaskGetNumberOfTasks() == SS_OWNER_SLOTS);
    invariant();
    puts("linked FreeRTOS admission: invalid affinity and 128-slot exhaustion unwind TLS/port resources; deletion permits buffer reuse");
    return 0;
}
static int fixture_retirement_pin(void)
{
    TaskHandle_t task = create(0, 1);
    assert(task);
    struct ss_kernel_pin pin;
    assert(ss_kernel_handle_pin(&ssKernelRegistry, task, &pin));
    unsigned slot = pin.slot;
    struct ssKernelMutation mutation = { .command = SS_K_DELETE, .slot = slot };
    assert(ss_control_publish(&ssKernelControl, slot, ssKernelMutate, &mutation) == 1);
    ssKernelControlDrain();
    assert(ssKernelControl.slot[slot].desired.dead);
    assert(ss_control_applied(&ssKernelControl, slot, 1));
    assert(atomic_load(&pin.descriptor->references) == 1);
    for (unsigned c = 0; c < configNUMBER_OF_CORES; ++c) {
        fixture_cpu = c;
        ssKernelControlDrain();
        assert(!atomic_load(&ssKernelControl.pending[c].word[slot / 64]));
    }
    assert(uxTaskGetNumberOfTasks() == 1);
    /* The task caller must be allowed to run until its final pin release.
     * That release, rather than repeated IRQs, restarts owner retirement. */
    ss_kernel_handle_put(&pin);
    for (unsigned c = 0; c < configNUMBER_OF_CORES; ++c) {
        assert(atomic_load(&ssKernelControl.pending[c].word[slot / 64]));
        fixture_cpu = c;
        ssKernelControlDrain();
    }
    ssKernelCleanup();
    assert(uxTaskGetNumberOfTasks() == 0);
    puts("linked FreeRTOS retirement: a retained caller pin emits no repeated owner hints; final release completes retirement");
    return 0;
}
static void fixture_dynamic_delete(TaskHandle_t task)
{
    fixture_cpu = configNUMBER_OF_CORES - 1;
    vTaskDelete(task);
    for (unsigned c = 0; c < configNUMBER_OF_CORES; ++c) {
        fixture_cpu = c;
        ssKernelControlDrain();
    }
    fixture_cpu = configNUMBER_OF_CORES - 1;
}
static int fixture_dynamic(void)
{
    for (unsigned c = 0; c < configNUMBER_OF_CORES; ++c) {
        TaskHandle_t idle = create(c, 0);
        xIdleTaskHandles[c] = pxCurrentTCBs[c] = idle;
        ((TCB_t *)idle)->uxTaskAttributes |= taskATTRIBUTE_IS_IDLE;
    }
    ssKernelBootstrap();
    xSchedulerRunning = pdTRUE;
    struct ss_owner_backend progress = ssKernelBackend;
    progress.wait = fixture_command_progress;
    ssKernelControl.backend = &progress;
    fixture_heap_protocol = true;
    TaskHandle_t handle;
    for (long allocation = 0; allocation < 2; ++allocation) {
        fixture_fail_after = allocation;
        handle = (TaskHandle_t)(uintptr_t)1;
        assert(xTaskCreate(fixture_entry, "alloc-fail", configMINIMAL_STACK_SIZE,
            NULL, 1, &handle) == errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY);
        assert(!handle && !fixture_live && !fixture_cleanup && !fixture_tls_deleted);
        fixture_fail_after = allocation;
        handle = (TaskHandle_t)(uintptr_t)1;
        assert(xTaskCreateAffinitySet(fixture_entry, "alloc-fail", configMINIMAL_STACK_SIZE,
            NULL, 1, 1, &handle) == errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY);
        assert(!handle && !fixture_live && !fixture_cleanup && !fixture_tls_deleted);
    }
    fixture_fail_after = -1;
    handle = (TaskHandle_t)(uintptr_t)1;
    assert(xTaskCreateAffinitySet(fixture_entry, "invalid", configMINIMAL_STACK_SIZE,
        NULL, 1, 0, &handle) == errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY);
    assert(!handle && !fixture_live && fixture_cleanup == 1 && fixture_tls_deleted == 1);
    assert(xTaskCreate(fixture_entry, "ordinary", configMINIMAL_STACK_SIZE,
        NULL, 1, &handle) == pdPASS && handle);
    assert(((TCB_t *)handle)->ucStaticallyAllocated == tskDYNAMICALLY_ALLOCATED_STACK_AND_TCB);
    assert(fixture_live == 2);
    fixture_dynamic_delete(handle);
    assert(!fixture_live && uxTaskGetNumberOfTasks() == configNUMBER_OF_CORES);
    assert(xTaskCreateAffinitySet(fixture_entry, "nohandle", configMINIMAL_STACK_SIZE,
        NULL, 1, 1, NULL) == pdPASS);
    handle = xTaskGetHandle("nohandle");
    assert(handle);
    fixture_dynamic_delete(handle);
    TaskHandle_t tasks[SS_OWNER_SLOTS - configNUMBER_OF_CORES];
    for (unsigned n = 0; n < SS_OWNER_SLOTS - configNUMBER_OF_CORES; ++n)
        assert(xTaskCreateAffinitySet(fixture_entry, "dynamic", configMINIMAL_STACK_SIZE,
            NULL, 1, 1, &tasks[n]) == pdPASS);
    assert(uxTaskGetNumberOfTasks() == SS_OWNER_SLOTS);
    unsigned cleaned = fixture_cleanup, deleted = fixture_tls_deleted;
    handle = (TaskHandle_t)(uintptr_t)1;
    assert(xTaskCreate(fixture_entry, "full", configMINIMAL_STACK_SIZE,
        NULL, 1, &handle) == errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY);
    assert(!handle);
    handle = (TaskHandle_t)(uintptr_t)1;
    assert(xTaskCreateAffinitySet(fixture_entry, "full", configMINIMAL_STACK_SIZE,
        NULL, 1, 1, &handle) == errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY);
    assert(!handle && fixture_live == 2 * (SS_OWNER_SLOTS - configNUMBER_OF_CORES));
    assert(fixture_cleanup == cleaned + 2 && fixture_tls_deleted == deleted + 2);
    unsigned slot = ((TCB_t *)tasks[0])->uxOwnerSlot;
    fixture_dynamic_delete(tasks[0]);
    assert(xTaskCreateAffinitySet(fixture_entry, "reuse", configMINIMAL_STACK_SIZE,
        NULL, 1, 1, &tasks[0]) == pdPASS);
    assert(((TCB_t *)tasks[0])->uxOwnerSlot == slot);
    for (unsigned n = 0; n < SS_OWNER_SLOTS - configNUMBER_OF_CORES; ++n) fixture_dynamic_delete(tasks[n]);
    assert(uxTaskGetNumberOfTasks() == configNUMBER_OF_CORES && !fixture_live);
    assert(fixture_allocations == fixture_frees);
    puts("linked FreeRTOS dynamic creation: allocation failures, invalid affinity, admission exhaustion, slot reuse and deletion balance stack/TCB allocations and TLS/port cleanup");
    return 0;
}
static int fixture_cleanup_shield(void)
{
    for (unsigned c = 0; c < configNUMBER_OF_CORES; ++c) {
        TaskHandle_t idle = create(c, 0);
        xIdleTaskHandles[c] = pxCurrentTCBs[c] = idle;
        ((TCB_t *)idle)->uxTaskAttributes |= taskATTRIBUTE_IS_IDLE;
    }
    TaskHandle_t executor = create(configNUMBER_OF_CORES, 5);
    TaskHandle_t donor = create(configNUMBER_OF_CORES + 1, 7);
    ssKernelBootstrap();
    xSchedulerRunning = pdTRUE;
    for (unsigned c = 0; c < configNUMBER_OF_CORES; ++c) enter(c);
    struct ssKernelTask *task = record(executor);
    assert(atomic_load(&task->owner) == 0 && atomic_load(&record(donor)->owner) == 1);
    fixture_cpu = 0;
    UBaseType_t irq = portSET_INTERRUPT_MASK();
    /* Model the protected executor interval after claiming one destructor. */
    atomic_store(&task->cleanup_shield, 1);
    struct ssKernelMutation deletion = { .command = SS_K_DELETE, .slot = ((TCB_t *)executor)->uxOwnerSlot };
    uint64_t sequence = ss_control_publish(&ssKernelControl, deletion.slot, ssKernelMutate, &deletion);
    ssKernelControlDrain();
    assert(task->cleanup_control_deferred && !task->dead);
    assert(atomic_load(&ssKernelRegistry.descriptor[deletion.slot].admission));
    assert(!ss_control_applied(&ssKernelControl, deletion.slot, sequence));
    unsigned interrupts = fixture_sgi[0];
    for (unsigned pass = 0; pass < 8; ++pass) ssKernelControlDrain();
    assert(fixture_sgi[0] == interrupts); /* No self-renewing retry storm. */
    portCLEAR_INTERRUPT_MASK(irq);
    fixture_cpu = 1;
    assert(ssKernelInherit(executor)); /* Pin-admissible despite pending delete. */
    fixture_cpu = 0;
    ssKernelControlDrain();
    assert(atomic_load(&task->effective) == 7 && !task->dead);
    ssKernelCleanupRelease(task);
    assert(!atomic_load(&task->cleanup_shield));
    assert(atomic_load(&ssKernelControl.pending[0].word[deletion.slot / 64]) &
           (UINT64_C(1) << (deletion.slot % 64)));
    for (unsigned pass = 0; pass < 3; ++pass)
        for (unsigned c = 0; c < configNUMBER_OF_CORES; ++c) enter(c);
    assert(atomic_load(&ssKernelCleanupCompleted[deletion.slot]) >= 1);
    assert(!ssKernelRegistry.registered[deletion.slot]);
    puts("linked FreeRTOS cleanup protection: pending deletion retains admission and priority inheritance, emits no retry storm, and completes after executor release");
    return 0;
}
/* A control issued before the scheduler starts is applied at the owner's first entry. The
 * logical state must follow it at once, as upstream FreeRTOS reports it. */
static int fixture_prestart_state(void)
{
    for (unsigned c = 0; c < configNUMBER_OF_CORES; ++c) {
        TaskHandle_t idle = create(c, 0);
        xIdleTaskHandles[c] = pxCurrentTCBs[c] = idle;
        ((TCB_t *)idle)->uxTaskAttributes |= taskATTRIBUTE_IS_IDLE;
        ((TCB_t *)idle)->uxCoreAffinityMask = (UBaseType_t)1 << c;
    }
    TaskHandle_t early = create(configNUMBER_OF_CORES, 5);
    assert(early && eTaskGetState(early) == eReady);
    vTaskSuspend(early);
    assert(eTaskGetState(early) == eSuspended);
    ssKernelBootstrap();
    xSchedulerRunning = pdTRUE;
    assert(eTaskGetState(early) == eSuspended); /* Before any owner has applied the control. */
    for (unsigned pass = 0; pass < 4; ++pass)
        for (unsigned c = 0; c < configNUMBER_OF_CORES; ++c) enter(c);
    assert(eTaskGetState(early) == eSuspended);
    fixture_cpu = 0;
    vTaskResume(early);
    assert(eTaskGetState(early) == eReady); /* Resume is visible before the owner applies it. */
    for (unsigned pass = 0; pass < 4; ++pass)
        for (unsigned c = 0; c < configNUMBER_OF_CORES; ++c) enter(c);
    assert(eTaskGetState(early) != eSuspended);
    puts("linked FreeRTOS pre-start controls: suspend and resume are visible in the task state before the owner applies them");
    return 0;
}
int main(int argc, char **argv)
{
    (void)argv;
    if (argc > 1 && !strcmp(argv[1], "shield")) return fixture_cleanup_shield();
    if (argc > 1 && !strcmp(argv[1], "prestart")) return fixture_prestart_state();
    if (argc > 1 && !strcmp(argv[1], "dynamic")) return fixture_dynamic();
    if (argc > 1 && !strcmp(argv[1], "retirement")) return fixture_retirement_pin();
    if (argc > 1 && !strcmp(argv[1], "admission")) return fixture_admission();
    if (argc > 1 && !strcmp(argv[1], "slice")) return fixture_slices();
    if (argc > 1) {
        int result = fixture_startup();
        if (!strcmp(argv[1], "end")) vTaskEndScheduler();
        return result;
    }
    /* Real static creation enters the registry and least-loaded placement. */
    for (unsigned c = 0; c < configNUMBER_OF_CORES; ++c) {
        TaskHandle_t idle = create(c, 0);
        struct ssKernelTask *s = record(idle);
        assert(atomic_load(&s->owner) == c);
        xIdleTaskHandles[c] = idle;
        ((TCB_t *)idle)->uxTaskAttributes |= taskATTRIBUTE_IS_IDLE;
        ((TCB_t *)idle)->uxCoreAffinityMask = (UBaseType_t)1 << c;
        pxCurrentTCBs[c] = idle;
    }
    ssKernelBootstrap();
    xSchedulerRunning = pdTRUE;
    TaskHandle_t task = create(configNUMBER_OF_CORES, 5);
    TaskHandle_t second = create(configNUMBER_OF_CORES + 1, 5);
    assert(atomic_load(&record(task)->owner) == 0);
    assert(atomic_load(&record(second)->owner) == 1);
    for (unsigned c = 0; c < configNUMBER_OF_CORES; ++c) enter(c);
    fixture_cpu = 0;
    assert(!strcmp(pcTaskGetName(task), "linked"));
    assert(vTaskCoreAffinityGet(task) == tskNO_AFFINITY);
    StackType_t *stack = NULL;
    StaticTask_t *tcb = NULL;
    assert(xTaskGetStaticBuffers(task, &stack, &tcb));
    assert(stack == stacks[configNUMBER_OF_CORES] && tcb == &storage[configNUMBER_OF_CORES]);
    vTaskSetTaskNumber(task, 42);
    assert(uxTaskGetTaskNumber(task) == 42);
    vTaskSetThreadLocalStoragePointer(task, 1, stack);
    assert(pvTaskGetThreadLocalStoragePointer(task, 1) == stack);
    assert(!pvTaskGetThreadLocalStoragePointer(task, -1));
    assert(!pvTaskGetThreadLocalStoragePointer(task, 2));
    vTaskSetApplicationTaskTag(task, fixture_hook);
    assert(xTaskGetApplicationTaskTag(task) == fixture_hook);
    assert(xTaskGetApplicationTaskTagFromISR(task) == fixture_hook);
    BaseType_t hook_value = 73;
    assert(xTaskCallApplicationTaskHook(task, &hook_value) == 73);
    assert(uxTaskGetStackHighWaterMark(task) == uxTaskGetStackHighWaterMark2(task));
    assert(xTaskGetSchedulerState() == taskSCHEDULER_RUNNING);
    vTaskSuspendAll();
    vTaskSuspendAll();
    assert(xTaskGetSchedulerState() == taskSCHEDULER_SUSPENDED);
    assert(ssKernelCPUs[0].preempt == 2);
    (void)xTaskResumeAll();
    assert(ssKernelCPUs[0].preempt == 1);
    (void)xTaskResumeAll();
    assert(!ssKernelCPUs[0].preempt);
    assert(atomic_load(&ssKernelControl.gate_phase) == SS_GATE_OPEN);
    TickType_t previous = xTaskGetTickCount();
    assert(xTaskPeriodicDelay(&previous, 2) == 0);
    enter(0);
    (void)xTaskIncrementTick();
    (void)xTaskIncrementTick();
    enter(0);
    assert(pxCurrentTCBs[0] == task);
    unsigned baseline_locks = fixture_locks;
    for (unsigned iteration = 0; iteration < 10000; ++iteration) {
        struct ssKernelTask *s = record(task);
        fixture_cpu = 0;
        assert(pxCurrentTCBs[0] == task);
        /* Block commit and remote notify use real notification compound state
         * and the public FreeRTOS notification entry point. */
        struct ss_notification_guard guard;
        ss_notification_gate_begin(&s->notification[0], &ssKernelBackend, &guard);
        uint64_t generation = s->generation + 1;
        assert(ss_notification_wait_enter(&guard, &s->notify_wait, generation,
                                          SS_NOTIFY_WAIT_COUNT, 0, true));
        s->notify_index = 0;
        ssKernelBlock(3, true, SS_K_NOTIFICATION);
        ss_notification_gate_end(&guard);
        if (iteration & 1) enter(0); /* Also wake before saved-frame eviction. */
        fixture_cpu = 1;
        BaseType_t woken = pdFALSE;
        assert(xTaskGenericNotifyFromISR(task, 0, 0, eIncrement, NULL, &woken) == pdPASS);
        enter(0);
        assert(pxCurrentTCBs[0] == task && !s->deadline.owner);
        ss_notification_gate_begin(&s->notification[0], &ssKernelBackend, &guard);
        struct ss_notification_finish finish = ss_notification_wait_finish(
            &guard, &s->notify_wait, generation, 0, false, false);
        ss_notification_gate_end(&guard);
        assert(finish.matched && finish.condition && finish.value == 1);
        assert(ulTaskGenericNotifyTake(0, pdTRUE, 0) == 0);
        /* Owner-local delay with CPU0 tick only posting remote owners. */
        vTaskDelay(2);
        enter(0);
        fixture_cpu = 0;
        (void)xTaskIncrementTick();
        assert(atomic_load(&s->execution) == SS_K_BLOCKED);
        (void)xTaskIncrementTick();
        enter(0);
        assert(pxCurrentTCBs[0] == task && !s->deadline.owner);
        invariant();
    }
    assert(fixture_locks == baseline_locks);
    fixture_cpu = 1;
    vTaskDelay(2);
    enter(1);
    fixture_cpu = 0;
    (void)xTaskIncrementTick();
    (void)xTaskIncrementTick();
    /* Remote CPU0 expiration delivery must leave CPU1's queue and heap alone. */
    assert(atomic_load(&record(second)->execution) == SS_K_BLOCKED);
    assert(record(second)->deadline.owner == &ssKernelCPUs[1].deadlines);
    enter(1);
    assert(pxCurrentTCBs[1] == second && !record(second)->deadline.owner);
    fixture_cpu = 0;
    List_t event;
    vListInitialise(&event);
    ssKernelObjectEnter();
    vTaskPlaceOnEventList(&event, 4);
    ssKernelObjectExit();
    enter(0);
    fixture_cpu = 1;
    UBaseType_t irq = ssKernelObjectEnterFromISR();
    (void)xTaskRemoveFromEventList(&event);
    ssKernelObjectExitFromISR(irq);
    enter(0);
    assert(pxCurrentTCBs[0] == task && listLIST_IS_EMPTY(&event));
    assert(!record(task)->event_reference.descriptor);
    assert(!record(task)->deadline.owner);
    /* Timeout removes an event registration before making the task ready. */
    /* A queue blocks under a suspension, which holds the task lock only: the insertion must still hold the interrupt
     * lock, which excludes the owner that times out another waiter of the same list. */
    fixture_insert_depth = 0;
    ssKernelObjectSuspend();
    vTaskPlaceOnEventList(&event, 1);
    (void)ssKernelObjectResume();
    assert(fixture_insert_depth >= 1);
    enter(0);
    (void)xTaskIncrementTick();
    enter(0);
    assert(listLIST_IS_EMPTY(&event) && pxCurrentTCBs[0] == task);
    fixture_ready_traces(task);
    fixture_objects();
    fixture_inheritance(task, second);
    fixture_diagnostics();
    fixture_critical();
    fixture_critical_commands(second);
    fixture_deferred_delete_migration(task);
    fixture_migration(task);
    fixture_migration_wake(task);
    fixture_delete_reuse(task, second);
    fixture_no_idle_pull(task);
    fixture_runtime_accounting(task);
    fixture_preemption(task);
    fixture_switch_hooks(task);
    fixture_api_contract(task);
    invariant();
    printf("linked FreeRTOS owner transitions: 10000 remote wakes, block-commit races, zero local scheduler locks, delay/event timeout\n");
    return 0;
}
