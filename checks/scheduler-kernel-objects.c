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
