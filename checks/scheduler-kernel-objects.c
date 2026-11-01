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
    fixture_yield_action = fixture_object_complete;
    assert(xQueueReceive(fixture_queue, &value, 3) == pdPASS);
    assert(value == 0x12345678 && fixture_object_wakes == 1);
    vQueueDelete(fixture_queue);
    fixture_queue = NULL;

    StaticEventGroup_t event_storage;
    fixture_events = xEventGroupCreateStatic(&event_storage);
    assert(fixture_events);
    fixture_yield_action = fixture_object_complete;
    assert(xEventGroupWaitBits(fixture_events, 4, pdTRUE, pdTRUE, 3) == 4);
    assert(xEventGroupGetBits(fixture_events) == 0 && fixture_object_wakes == 2);
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
