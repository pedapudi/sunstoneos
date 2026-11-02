/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Public object APIs run against real lists and tasks. A yield action explicitly
 * advances saved-frame checkpoints; this is not concurrent-stack execution. */
#include "queue.h"
#include "event_groups.h"
#include "stream_buffer.h"
#include "timers.h"
static void (*fixture_yield_action)(void);
void ssFixtureYield(void)
{
    void (*action)(void) = fixture_yield_action;
    fixture_yield_action = NULL;
    if (action) action();
}
static QueueHandle_t fixture_queue;
static EventGroupHandle_t fixture_events;
static StreamBufferHandle_t fixture_stream;
static unsigned fixture_object_wakes;
static void fixture_object_complete(void)
{
    assert(atomic_load(&ssKernelControl.gate_phase) == SS_GATE_OPEN);
    assert(!ssKernelCPUs[0].object_depth && !ssKernelCPUs[0].preempt);
    enter(0);
    assert(atomic_load(&ssKernelCurrent()->execution) == SS_K_RUNNING);
    fixture_cpu = 1;
    uint32_t value = 0x12345678;
    if (fixture_queue) assert(xQueueSend(fixture_queue, &value, 0) == pdPASS);
    if (fixture_events) assert(xEventGroupSetBits(fixture_events, 4) == 0);
    if (fixture_stream) assert(xStreamBufferSend(fixture_stream, &value, sizeof(value), 0) == sizeof(value));
    enter(0);
    ++fixture_object_wakes;
}
static void fixture_timer(TimerHandle_t timer) { (void)timer; }
static void fixture_objects(void)
{
    fixture_cpu = 0;
    StaticQueue_t queue_storage;
    uint8_t queue_bytes[2 * sizeof(uint32_t)];
    fixture_queue = xQueueCreateStatic(2, sizeof(uint32_t), queue_bytes, &queue_storage);
    assert(fixture_queue);
    uint32_t value = 0;
    assert(xQueueReceive(fixture_queue, &value, 0) == pdFAIL);
    unsigned waiter_slot = ssKernelCurrent()->ready.slot;
    unsigned ready_before = fixture_ready_count[waiter_slot];
    fixture_yield_action = fixture_object_complete;
    assert(xQueueReceive(fixture_queue, &value, 3) == pdPASS);
    assert(value == 0x12345678 && fixture_object_wakes == 1);
    assert(fixture_ready_count[waiter_slot] == ready_before + 1 && fixture_ready_cpu[waiter_slot] == 1);
    vQueueDelete(fixture_queue);
    fixture_queue = NULL;

    StaticEventGroup_t event_storage;
    fixture_events = xEventGroupCreateStatic(&event_storage);
    assert(fixture_events);
    ready_before = fixture_ready_count[waiter_slot];
    fixture_yield_action = fixture_object_complete;
    assert(xEventGroupWaitBits(fixture_events, 4, pdTRUE, pdTRUE, 3) == 4);
    assert(xEventGroupGetBits(fixture_events) == 0 && fixture_object_wakes == 2);
    assert(fixture_ready_count[waiter_slot] == ready_before + 1 && fixture_ready_cpu[waiter_slot] == 1);
    vEventGroupDelete(fixture_events);
    fixture_events = NULL;

    StaticStreamBuffer_t stream_storage;
    uint8_t stream_bytes[16];
    fixture_stream = xStreamBufferCreateStatic(sizeof(stream_bytes), 1, stream_bytes, &stream_storage);
    assert(fixture_stream);
    fixture_yield_action = fixture_object_complete;
    value = 0;
    assert(xStreamBufferReceive(fixture_stream, &value, sizeof(value), 3) == sizeof(value));
    assert(value == 0x12345678 && fixture_object_wakes == 3);
    vStreamBufferDelete(fixture_stream);
    fixture_stream = NULL;

    StaticTimer_t timer_storage;
    TimerHandle_t timer = xTimerCreateStatic("linked-timer", 2, pdFALSE, NULL,
                                             fixture_timer, &timer_storage);
    assert(timer && xTimerStart(timer, 0) == pdPASS);
    assert(xTimerDelete(timer, 0) == pdPASS);
    assert(atomic_load(&ssKernelControl.gate_phase) == SS_GATE_OPEN);
    assert(!ssKernelCPUs[0].preempt && !ssKernelCPUs[0].object_depth);
    printf("linked FreeRTOS objects: queue retry, event wait/set, stream wait/send, timer command enqueue; no global suspension\n");
}

static void fixture_inheritance(TaskHandle_t holder, TaskHandle_t donor)
{
    fixture_cpu = 0;
    vTaskPrioritySet(holder, 2);
    enter(0);
    assert(uxTaskPriorityGet(holder) == 2);
    ssKernelObjectEnter();
    assert(pvTaskIncrementMutexHeldCount() == holder);
    assert(pvTaskIncrementMutexHeldCount() == holder);
    ssKernelObjectExit();
    fixture_cpu = 1;
    assert(pxCurrentTCBs[1] == donor && uxTaskPriorityGet(donor) == 5);
    ssKernelObjectEnter();
    assert(xTaskPriorityInherit(holder));
    /* Publication is deferred until the object metadata lock is released. */
    assert(uxTaskPriorityGet(holder) == 2);
    ssKernelObjectExit();
    enter(0);
    assert(uxTaskPriorityGet(holder) == 5 && uxTaskBasePriorityGet(holder) == 2);
    ssKernelObjectEnter();
    assert(!xTaskPriorityDisinherit(holder));
    ssKernelObjectExit();
    enter(0);
    assert(uxTaskPriorityGet(holder) == 5); /* Nested mutex retains donation. */
    fixture_cpu = 1;
    ssKernelObjectEnter();
    vTaskPriorityDisinheritAfterTimeout(holder, 3);
    ssKernelObjectExit();
    enter(0);
    assert(uxTaskPriorityGet(holder) == 3);
    ssKernelObjectEnter();
    assert(xTaskPriorityDisinherit(holder));
    ssKernelObjectExit();
    enter(0);
    assert(uxTaskPriorityGet(holder) == 2 && !atomic_load(&record(holder)->mutexes));
    vTaskPrioritySet(holder, 5);
    enter(0);
    assert(uxTaskPriorityGet(holder) == 5);
    printf("linked FreeRTOS inheritance: deferred owner repair, nested mutex retention, timeout lowering, final disinherit\n");
}

static unsigned fixture_port_starts;
BaseType_t xPortStartScheduler(void)
{
    ++fixture_port_starts;
    assert(xSchedulerRunning);
    for (unsigned cpu = 0; cpu < configNUMBER_OF_CORES; ++cpu) {
        assert(pxCurrentTCBs[cpu] == xIdleTaskHandles[cpu]);
        assert(ssKernelCPUs[cpu].run.current->context == xIdleTaskHandles[cpu]);
    }
    return pdFALSE;
}
static void fixture_diagnostic_visit(TaskHandle_t task, eTaskState state, void *opaque)
{
    unsigned *count = opaque;
    TaskStatus_t status;
    vTaskGetInfo(task, &status, pdTRUE, eInvalid);
    assert(status.xHandle == task && status.eCurrentState == state);
    assert(status.pcTaskName && status.pxStackBase && status.xTaskNumber);
    ++*count;
}
static void fixture_diagnostics(void)
{
    fixture_cpu = 0;
    TaskStatus_t status[SS_OWNER_SLOTS];
    configRUN_TIME_COUNTER_TYPE total = 99;
    UBaseType_t count = uxTaskGetNumberOfTasks();
    assert(uxTaskGetSystemState(status, count - 1, &total) == 0);
    assert(uxTaskGetSystemState(status, SS_OWNER_SLOTS, &total) == count);
    assert(total == 0);
    unsigned visits = 0;
    assert(uxTaskCallForEachTask(fixture_diagnostic_visit, &visits, &total) == count);
    assert(visits == count);
    assert(!ssKernelCPUs[0].preempt && atomic_load(&ssKernelControl.gate_phase) == SS_GATE_OPEN);
    assert(xTaskGetCurrentTaskHandle() == pxCurrentTCBs[0]);
    assert(xTaskGetCurrentTaskHandleForCore(1) == pxCurrentTCBs[1]);
    assert(!xTaskGetCurrentTaskHandleForCore(-1));
    assert(!xTaskGetCurrentTaskHandleForCore(configNUMBER_OF_CORES));
}
static int fixture_startup(void)
{
    vTaskResetState();
    vTaskStartScheduler();
    assert(fixture_port_starts == 1 && uxTaskGetNumberOfTasks() == configNUMBER_OF_CORES + 1);
    assert(xTimerGetTimerDaemonTaskHandle());
    for (unsigned cpu = 0; cpu < configNUMBER_OF_CORES; ++cpu) enter(cpu);
    assert(pxCurrentTCBs[0] == xTimerGetTimerDaemonTaskHandle());
    fixture_diagnostics();
    invariant();
    printf("linked FreeRTOS startup: public scheduler start creates pinned idle owners and timer task; registry diagnostics\n");
    return 0;
}

static void fixture_critical(void)
{
    fixture_cpu = 0;
    unsigned locks = fixture_locks;
    vTaskEnterCritical();
    vTaskEnterCritical();
    assert(ullCriticalNestings[0] == 2 && ssKernelCPUs[0].preempt == 1);
    assert(atomic_load(&ssKernelControl.gate_phase) == SS_GATE_OPEN);
    unsigned held_locks = fixture_locks;
    enter(1); /* Public application exclusion does not stop another owner. */
    assert(fixture_locks == held_locks);
    fixture_cpu = 0;
    vTaskExitCritical();
    assert(ullCriticalNestings[0] == 1 && ssKernelCPUs[0].preempt == 1);
    vTaskExitCritical();
    assert(!ullCriticalNestings[0] && !ssKernelCPUs[0].preempt);
    assert(fixture_locks == locks + 2);
    UBaseType_t first = vTaskEnterCriticalFromISR();
    UBaseType_t second = vTaskEnterCriticalFromISR();
    assert(ullCriticalNestings[0] == 2 && ssKernelCPUs[0].preempt == 2);
    vTaskExitCriticalFromISR(second);
    vTaskExitCriticalFromISR(first);
    assert(!ullCriticalNestings[0] && !ssKernelCPUs[0].preempt);
    assert(fixture_locks == locks + 3);
    printf("linked FreeRTOS critical APIs: nested task/ISR exclusion preserves port nesting and permits remote owner dispatch\n");
}

static TaskHandle_t fixture_progress_task;
static void fixture_control_progress(void *opaque)
{
    (void)opaque;
    for (unsigned cpu = 0; cpu < configNUMBER_OF_CORES; ++cpu) enter(cpu);
    fixture_cpu = atomic_load(&record(fixture_progress_task)->owner);
}
static void fixture_migration(TaskHandle_t task)
{
    struct ss_owner_backend progress = ssKernelBackend;
    progress.wait = fixture_control_progress;
    ssKernelControl.backend = &progress;
    fixture_progress_task = task;
    fixture_cpu = 0;
    vTaskCoreAffinitySet(task, (UBaseType_t)1 << 2);
    assert(fixture_cpu == 2 && pxCurrentTCBs[2] == task);
    assert(atomic_load(&record(task)->owner) == 2);
    assert(vTaskCoreAffinityGet(task) == (UBaseType_t)1 << 2);
    assert(atomic_load(&record(task)->execution) == SS_K_RUNNING);
    vTaskCoreAffinitySet(task, (UBaseType_t)1 << 0);
    assert(fixture_cpu == 0 && pxCurrentTCBs[0] == task);
    /* Drain the final source forwarding acknowledgment after route closure. */
    for (unsigned cpu = 0; cpu < configNUMBER_OF_CORES; ++cpu) enter(cpu);
    ssKernelControl.backend = &ssKernelBackend;
    fixture_cpu = 0;
    invariant();
    printf("linked FreeRTOS migration: public self-affinity transfer and return preserve single owner after adoption\n");
}

static void fixture_migration_wake(TaskHandle_t task)
{
    fixture_cpu = 0;
    struct ssKernelTask *s = record(task);
    struct ss_notification_guard guard;
    ss_notification_gate_begin(&s->notification[0], &ssKernelBackend, &guard);
    uint64_t generation = s->generation + 1;
    assert(ss_notification_wait_enter(&guard, &s->notify_wait, generation,
                                      SS_NOTIFY_WAIT_COUNT, 0, true));
    s->notify_index = 0;
    ssKernelBlock(3, true, SS_K_NOTIFICATION);
    ss_notification_gate_end(&guard);
    enter(0); /* Saved frame leaves CPU0 before migration begins. */
    fixture_cpu = 1;
    assert(ssKernelCommand(task, SS_K_AFFINITY, (UBaseType_t)1 << 2, true));
    enter(0); /* Close the old route and detach, without destination adoption. */
    assert(atomic_load(&s->execution) == SS_K_TRANSFERRING);
    assert(atomic_load(&s->owner) == 0 && !s->ready.queued && !s->deadline.owner);
    fixture_cpu = 1;
    BaseType_t woken = pdFALSE;
    assert(xTaskGenericNotifyFromISR(task, 0, 0, eIncrement, NULL, &woken) == pdPASS);
    /* A closed route broadcasts ordinary owner hints; neither owner can make
     * the task ready before adoption has installed queue/deadline ownership. */
    for (unsigned cpu = 0; cpu < configNUMBER_OF_CORES; ++cpu)
        assert(atomic_load(&ssKernelCPUs[cpu].run.incoming[s->ready.priority][s->ready.slot / 64]) &
               (UINT64_C(1) << (s->ready.slot % 64)));
    enter(0);
    assert(atomic_load(&s->execution) == SS_K_TRANSFERRING && !s->ready.queued);
    enter(2);
    assert(pxCurrentTCBs[2] == task && !s->deadline.owner);
    assert(atomic_load(&s->owner) == 2 && atomic_load(&s->execution) == SS_K_RUNNING);
    ss_notification_gate_begin(&s->notification[0], &ssKernelBackend, &guard);
    struct ss_notification_finish finish = ss_notification_wait_finish(
        &guard, &s->notify_wait, generation, 0, false, false);
    ss_notification_gate_end(&guard);
    assert(finish.matched && finish.condition && finish.value == 1);
    for (unsigned cpu = 0; cpu < configNUMBER_OF_CORES; ++cpu) enter(cpu);
    struct ss_owner_backend progress = ssKernelBackend;
    progress.wait = fixture_control_progress;
    ssKernelControl.backend = &progress;
    fixture_progress_task = task;
    fixture_cpu = 2;
    vTaskCoreAffinitySet(task, 1);
    ssKernelControl.backend = &ssKernelBackend;
    fixture_cpu = 0;
    invariant();
    printf("linked FreeRTOS migration wake: closed-route broadcast cannot activate before adoption; notification survives transfer\n");
}

static void fixture_delete_reuse(TaskHandle_t task, TaskHandle_t survivor)
{
    unsigned slot = record(task)->ready.slot;
    uint64_t incarnation = atomic_load(&ssKernelRegistry.descriptor[slot].incarnation);
    struct ss_owner_backend progress = ssKernelBackend;
    progress.wait = fixture_control_progress;
    ssKernelControl.backend = &progress;
    fixture_progress_task = survivor;
    fixture_cpu = 1;
    vTaskDelete(task);
    for (unsigned round = 0; round < 3; ++round)
        for (unsigned cpu = 0; cpu < configNUMBER_OF_CORES; ++cpu) enter(cpu);
    struct ss_kernel_pin pin;
    assert(!ssKernelPin(task, &pin));
    fixture_cpu = 1;
    TaskHandle_t replacement = create(configNUMBER_OF_CORES, 5);
    assert(replacement == task);
    assert(ssKernelPin(replacement, &pin));
    assert(pin.incarnation > incarnation);
    ss_kernel_handle_put(&pin);
    for (unsigned cpu = 0; cpu < configNUMBER_OF_CORES; ++cpu) enter(cpu);
    assert(pxCurrentTCBs[0] == replacement && atomic_load(&record(replacement)->owner) == 0);
    ssKernelControl.backend = &ssKernelBackend;
    fixture_cpu = 0;
    invariant();
    printf("linked FreeRTOS deletion: public delete retires all owner hints before static TCB storage reuse\n");
}

static unsigned fixture_runtime;
unsigned ssFixtureRuntime(void) { return fixture_runtime; }
static void fixture_runtime_accounting(TaskHandle_t task)
{
    fixture_cpu = 0;
    assert(xTaskGetHandle("linked"));
    assert(!xTaskGetHandle("missing"));
    for (unsigned cpu = 0; cpu < configNUMBER_OF_CORES; ++cpu)
        assert(xTaskGetIdleTaskHandleForCore(cpu) == xIdleTaskHandles[cpu]);
    fixture_runtime = 1000;
    enter(0);
    assert(ulTaskGetRunTimeCounter(task) == 1000);
    fixture_runtime = 1100;
    assert(ulTaskGetRunTimeCounter(task) == 1100);
    assert(ulTaskGetRunTimePercent(task) == 100);
    assert(ulTaskGetIdleRunTimeCounter() == 6600);
    assert(ulTaskGetIdleRunTimePercent() == 75);
    TaskStatus_t status;
    vTaskGetInfo(task, &status, pdFALSE, eInvalid);
    assert(status.ulRunTimeCounter == 1100);
    printf("linked FreeRTOS optional runtime: switched/current intervals, task/idle percentage and pinned info snapshot\n");
}

static void fixture_preemption(TaskHandle_t task)
{
    fixture_cpu = 0;
    vTaskPreemptionDisable(task);
    assert(((TCB_t *)task)->xPreemptionDisable);
    TaskHandle_t urgent = xTaskCreateStaticAffinitySet(fixture_entry, "urgent", configMINIMAL_STACK_SIZE,
        NULL, 6, stacks[configNUMBER_OF_CORES + 2], &storage[configNUMBER_OF_CORES + 2], 1);
    assert(urgent);
    enter(0);
    assert(pxCurrentTCBs[0] == task); /* Higher ready priority cannot preempt. */
    vTaskDelay(1); /* Disable never prevents the task from blocking. */
    enter(0);
    assert(pxCurrentTCBs[0] == urgent);
    vTaskSuspend(urgent);
    enter(0);
    assert(pxCurrentTCBs[0] == xIdleTaskHandles[0]);
    (void)xTaskIncrementTick();
    enter(0);
    assert(pxCurrentTCBs[0] == task);
    /* Explicit yield, unlike involuntary preemption, may select urgent work. */
    vTaskResume(urgent);
    ssKernelOwnerYield();
    enter(0);
    assert(pxCurrentTCBs[0] == urgent);
    vTaskSuspend(urgent);
    enter(0);
    assert(pxCurrentTCBs[0] == task);
    vTaskPreemptionEnable(task);
    assert(!((TCB_t *)task)->xPreemptionDisable);
    vTaskResume(urgent);
    enter(0);
    assert(pxCurrentTCBs[0] == urgent);
    /* Synchronous remote change must be acknowledged by its owner. */
    struct ss_owner_backend progress = ssKernelBackend;
    progress.wait = fixture_control_progress;
    ssKernelControl.backend = &progress;
    fixture_progress_task = task;
    fixture_cpu = 1;
    vTaskPreemptionDisable(task);
    assert(((TCB_t *)task)->xPreemptionDisable);
    ssKernelControl.backend = &ssKernelBackend;
    fixture_cpu = 0;
    vTaskSuspend(urgent);
    enter(0);
    assert(pxCurrentTCBs[0] == task);
    vTaskPreemptionEnable(task);
    printf("linked FreeRTOS optional preemption: owner acknowledgment, higher-priority deferral, explicit yield and blocking\n");
}

void vPortEndScheduler(void)
{
    /* Mirror the pinned ARM_CR82 unsupported-stop assertion, and prove the
     * task wrapper did not delete idle/timer tasks before reaching the port. */
    assert(xSchedulerRunning && uxTaskGetNumberOfTasks() == configNUMBER_OF_CORES + 1);
    fprintf(stderr, "scheduler stop unsupported\n");
    configASSERT(NULL);
}

static uintptr_t fixture_tls[configNUMBER_OF_CORES];
static unsigned fixture_tls_deleted, fixture_switches[configNUMBER_OF_CORES], fixture_traces[2];
void ssFixtureTLS(uintptr_t block) { fixture_tls[fixture_cpu] = block; }
void ssFixtureTLSDelete(uintptr_t block) { assert(block); ++fixture_tls_deleted; }
void ssFixtureSwitch(void *task)
{
    assert(task == pxCurrentTCBs[fixture_cpu]);
    ++fixture_switches[fixture_cpu];
}
void ssFixtureTrace(unsigned phase) { assert(phase < 2); ++fixture_traces[phase]; }
static void fixture_switch_hooks(TaskHandle_t task)
{
    fixture_cpu = 0;
    assert(pxCurrentTCBs[0] == task && fixture_tls_deleted);
    unsigned switches = fixture_switches[0], outgoing = fixture_traces[0], incoming = fixture_traces[1];
    FreeRTOS_errno = 17;
    TaskHandle_t urgent = xTaskCreateStaticAffinitySet(fixture_entry, "hook", configMINIMAL_STACK_SIZE,
        NULL, 6, stacks[configNUMBER_OF_CORES + 3], &storage[configNUMBER_OF_CORES + 3], 1);
    assert(urgent);
    enter(0);
    assert(pxCurrentTCBs[0] == urgent && FreeRTOS_errno == 0);
    assert(((TCB_t *)task)->iTaskErrno == 17);
    assert(fixture_tls[0] == ((TCB_t *)urgent)->xTLSBlock);
    FreeRTOS_errno = 23;
    vTaskSuspend(urgent);
    enter(0);
    assert(pxCurrentTCBs[0] == task && FreeRTOS_errno == 17);
    assert(((TCB_t *)urgent)->iTaskErrno == 23);
    assert(fixture_tls[0] == ((TCB_t *)task)->xTLSBlock);
    assert(fixture_switches[0] == switches + 2);
    assert(fixture_traces[0] == outgoing + 2 && fixture_traces[1] == incoming + 2);
    printf("linked FreeRTOS switch hooks: trace, task hook, TLS selection/deletion and errno save/restore\n");
}

static void fixture_no_idle_pull(TaskHandle_t task)
{
    StaticTask_t background_tcb;
    StackType_t background_stack[configMINIMAL_STACK_SIZE];
    fixture_cpu = 0;
    TaskHandle_t background = xTaskCreateStaticAffinitySet(fixture_entry, "background", configMINIMAL_STACK_SIZE,
        NULL, 1, background_stack, &background_tcb, 1);
    assert(background);
    enter(0);
    enter(2);
    unsigned imbalance_observations = 0;
    if (record(background)->ready.queued && atomic_load(&record(background)->owner) == 0 &&
        pxCurrentTCBs[0] == task && pxCurrentTCBs[2] == xIdleTaskHandles[2])
        ++imbalance_observations;
    assert(imbalance_observations == 1); /* Idle pull begins only in series D. */
    struct ss_owner_backend progress = ssKernelBackend;
    progress.wait = fixture_control_progress;
    ssKernelControl.backend = &progress;
    fixture_progress_task = task;
    fixture_cpu = 1;
    vTaskDelete(background);
    for (unsigned round = 0; round < 3; ++round)
        for (unsigned cpu = 0; cpu < configNUMBER_OF_CORES; ++cpu) enter(cpu);
    struct ss_kernel_pin pin;
    assert(!ssKernelPin(background, &pin));
    ssKernelControl.backend = &ssKernelBackend;
    fixture_cpu = 0;
    printf("linked FreeRTOS placement: %u checkpoint observations of ready work on a busy owner beside an idle core; no idle pull\n", imbalance_observations);
}

static int fixture_slices(void)
{
    vTaskResetState();
    vTaskStartScheduler();
    for (unsigned cpu = 0; cpu < configNUMBER_OF_CORES; ++cpu) enter(cpu);
    static StaticTask_t slice_tcb[2];
    static StackType_t slice_stack[2][configMINIMAL_STACK_SIZE];
    fixture_cpu = 2;
    TaskHandle_t first = xTaskCreateStaticAffinitySet(fixture_entry, "slice-a", configMINIMAL_STACK_SIZE,
        NULL, 4, slice_stack[0], &slice_tcb[0], 1u << 2);
    enter(2);
    assert(pxCurrentTCBs[2] == first);
    unsigned idle_sgi = fixture_sgi[3];
    assert(atomic_load(&ssKernelCPUs[2].slice_deadline) == UINT64_MAX);
    unsigned single_sgi = fixture_sgi[2];
    fixture_cpu = 0;
    (void)xTaskIncrementTick();
    assert(fixture_sgi[2] == single_sgi);
    fixture_cpu = 2;
    TaskHandle_t second = xTaskCreateStaticAffinitySet(fixture_entry, "slice-b", configMINIMAL_STACK_SIZE,
        NULL, 4, slice_stack[1], &slice_tcb[1], 1u << 2);
    assert(pxCurrentTCBs[2] == first && second);
    assert(ssKernelCPUs[2].deadlines.count == 0);
    /* Local creation arms the first peer without an intervening owner IRQ. */
    assert(atomic_load(&ssKernelCPUs[2].slice_deadline) == ssKernelNow() + 1);
    for (unsigned slice = 0; slice < 6; ++slice) {
        unsigned before = fixture_sgi[2];
        fixture_cpu = 0;
        (void)xTaskIncrementTick();
        assert(fixture_sgi[2] == before + 1);
        fixture_cpu = 2;
        assert(ssKernelOwnerIRQExit());
        enter(2);
        assert(pxCurrentTCBs[2] == ((slice & 1) ? first : second));
        assert(!ssKernelCPUs[2].deadlines.count);
    }
    fixture_cpu = 2;
    vTaskPreemptionDisable(first);
    assert(atomic_load(&ssKernelCPUs[2].slice_deadline) == UINT64_MAX);
    unsigned disabled_sgi = fixture_sgi[2];
    for (unsigned tick = 0; tick < 3; ++tick) {
        fixture_cpu = 0;
        (void)xTaskIncrementTick();
        assert(fixture_sgi[2] == disabled_sgi);
    }
    fixture_cpu = 2;
    vTaskPreemptionEnable(first);
    assert(atomic_load(&ssKernelCPUs[2].slice_deadline) == ssKernelNow() + 1);
    /* Removing the only peer disarms rather than sending idle-period SGIs. */
    vTaskSuspend(second);
    assert(atomic_load(&ssKernelCPUs[2].slice_deadline) == UINT64_MAX);
    unsigned lone_sgi = fixture_sgi[2];
    fixture_cpu = 0;
    (void)xTaskIncrementTick();
    assert(fixture_sgi[2] == lone_sgi);
    fixture_cpu = 2;
    vTaskResume(second);
    assert(atomic_load(&ssKernelCPUs[2].slice_deadline) == ssKernelNow() + 1);
    vTaskPrioritySet(second, 3);
    assert(atomic_load(&ssKernelCPUs[2].slice_deadline) == UINT64_MAX);
    vTaskPrioritySet(second, 4);
    assert(atomic_load(&ssKernelCPUs[2].slice_deadline) == ssKernelNow() + 1);
    assert(fixture_sgi[3] == idle_sgi);
    printf("linked FreeRTOS slicing: secondary equal-priority FIFO rotation without timeout deadlines; single/disabled owners receive no slice SGIs\n");
    return 0;
}

void ssFixtureReady(void *task)
{
    TCB_t *t = task;
    unsigned slot = t->uxOwnerSlot;
    assert(slot < SS_OWNER_SLOTS);
    ++fixture_ready_count[slot];
    fixture_ready_cpu[slot] = fixture_cpu;
    /* A ready trace may run in the waker's ISR or under a control FIFO. Its
     * current-task query must be a nonblocking snapshot, never a global gate. */
    assert(xTaskGetCurrentTaskHandleForCore(fixture_cpu) == pxCurrentTCBs[fixture_cpu]);
    if (task == fixture_ready_watch) {
        uint64_t word = atomic_load(&ssKernelTasks[slot].notification[0].word);
        assert(fixture_cpu == 1 && ss_notification_payload_state(word) == SS_NOTIFY_RECEIVED);
        assert((word & SS_NOTIFICATION_VALUE) == 1 && (word & SS_NOTIFICATION_COMPLETED));
        fixture_ready_payload = true;
    }
}
static void fixture_trace_wait(struct ssKernelTask *s)
{
    struct ss_notification_guard guard;
    ss_notification_gate_begin(&s->notification[0], &ssKernelBackend, &guard);
    uint64_t generation = s->generation + 1;
    assert(ss_notification_wait_enter(&guard, &s->notify_wait, generation,
                                      SS_NOTIFY_WAIT_COUNT, 0, true));
    s->notify_index = 0;
    ssKernelBlock(3, true, SS_K_NOTIFICATION);
    ss_notification_gate_end(&guard);
}
static void fixture_ready_traces(TaskHandle_t task)
{
    fixture_cpu = 0;
    struct ssKernelTask *s = record(task);
    unsigned slot = s->ready.slot;
    fixture_trace_wait(s);
    enter(0);
    fixture_ready_baseline = fixture_ready_count[slot];
    fixture_ready_watch = task;
    fixture_ready_payload = false;
    fixture_cpu = 1;
    BaseType_t woken = pdFALSE;
    assert(xTaskGenericNotifyFromISR(task, 0, 0, eIncrement, NULL, &woken) == pdPASS);
    assert(fixture_ready_payload && fixture_ready_count[slot] == fixture_ready_baseline + 1);
    assert(xTaskGenericNotifyFromISR(task, 0, 0, eSetBits, NULL, &woken) == pdPASS);
    assert(fixture_ready_count[slot] == fixture_ready_baseline + 1);
    fixture_ready_watch = NULL;
    enter(0);
    assert(fixture_ready_count[slot] == fixture_ready_baseline + 1 && fixture_ready_cpu[slot] == 1);
    struct ss_notification_guard guard;
    ss_notification_gate_begin(&s->notification[0], &ssKernelBackend, &guard);
    struct ss_notification_finish finish = ss_notification_wait_finish(
        &guard, &s->notify_wait, s->generation, 0, false, false);
    ss_notification_gate_end(&guard);
    assert(finish.matched && finish.value == 1);
    fixture_trace_wait(s);
    enter(0);
    unsigned before_timeout = fixture_ready_count[slot];
    for (unsigned tick = 0; tick < 3; ++tick) (void)xTaskIncrementTick();
    assert(fixture_ready_count[slot] == before_timeout + 1 && fixture_ready_cpu[slot] == 0);
    fixture_cpu = 1;
    assert(xTaskGenericNotifyFromISR(task, 0, 0, eIncrement, NULL, &woken) == pdPASS);
    assert(fixture_ready_count[slot] == before_timeout + 1);
    enter(0);
    ss_notification_gate_begin(&s->notification[0], &ssKernelBackend, &guard);
    finish = ss_notification_wait_finish(&guard, &s->notify_wait, s->generation, 0, false, true);
    ss_notification_gate_end(&guard);
    assert(finish.matched && finish.value == 1);
    vTaskSuspend(task);
    enter(0);
    unsigned before_resume = fixture_ready_count[slot];
    struct ss_owner_backend progress = ssKernelBackend;
    progress.wait = fixture_control_progress;
    ssKernelControl.backend = &progress;
    fixture_progress_task = task;
    fixture_cpu = 1;
    vTaskResume(task);
    assert(fixture_ready_count[slot] == before_resume + 1 && fixture_ready_cpu[slot] == 1);
    vTaskResume(task);
    assert(fixture_ready_count[slot] == before_resume + 1);
    ssKernelControl.backend = &ssKernelBackend;
    fixture_cpu = 0;
    for (unsigned notify_first = 0; notify_first < 2; ++notify_first) {
        fixture_trace_wait(s);
        enter(0);
        unsigned before_abort = fixture_ready_count[slot];
        ssKernelControl.backend = &progress;
        fixture_progress_task = task;
        fixture_cpu = 1;
        if (notify_first)
            assert(xTaskGenericNotifyFromISR(task, 0, 0, eIncrement, NULL, &woken) == pdPASS);
        assert(xTaskAbortDelay(task));
        fixture_cpu = 1;
        if (!notify_first)
            assert(xTaskGenericNotifyFromISR(task, 0, 0, eIncrement, NULL, &woken) == pdPASS);
        assert(fixture_ready_count[slot] == before_abort + 1);
        enter(0);
        ss_notification_gate_begin(&s->notification[0], &ssKernelBackend, &guard);
        finish = ss_notification_wait_finish(&guard, &s->notify_wait, s->generation, 0, false, true);
        ss_notification_gate_end(&guard);
        assert(finish.matched && finish.value == 1);
        TimeOut_t timeout;
        TickType_t remaining = 3;
        vTaskSetTimeOutState(&timeout);
        assert(xTaskCheckForTimeOut(&timeout, &remaining)); /* Clears abort latch. */
        ssKernelControl.backend = &ssKernelBackend;
    }
    printf("linked FreeRTOS ready traces: caller CPU and committed notification payload precede SGI; one hook per notify/timeout/resume generation\n");
}
