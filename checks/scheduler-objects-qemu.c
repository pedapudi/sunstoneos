/* Kernel object locks on eight emulated cores.
 *
 * Phase 1, independence. One core holds the lock of a queue open (a trace hook inside the queue's critical section
 * spins) until each of six other cores has completed a number of operations on queues of its own, or until a budget of
 * the holder's own loop iterations runs out. An operation on another object must not wait for the held one, so the
 * others finish while the hold is still open and the budget never runs out. With one lock shared by every object, as in
 * the global-lock kernel, the others cannot start, the budget runs out on every hold and the check fails. The criterion
 * counts operations and the holder's iterations, not time, so the emulator's scheduling of its cores cannot change it.
 *
 * Phase 2, integrity. Producers, consumers with short timeouts (so that blocked waiters expire while senders run),
 * interrupt-level senders on every core, a mutex with priority inheritance, an event group, a stream buffer and a
 * counting semaphore share objects across cores. No message may be lost or duplicated, no per-source order may break,
 * and the mutex must exclude. The cores run on the instruction counter, so a run is repeatable. */
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include "event_groups.h"
#include "stream_buffer.h"
#include "platform.h"
#include "perturb.h"
#include <stdbool.h>
#include <stdint.h>

#define CORES configNUMBER_OF_CORES
_Static_assert(CORES == 8, "the check needs the eight-core configuration");
_Static_assert(configSS_PARTITIONED_SCHEDULER == 1, "exercise the private fork");

extern uint64_t ss_wake_period;
volatile unsigned ss_wake_stop;
void ss_kernel_test_admin_enter(unsigned cpu) { (void)cpu; }
static uint64_t frequency(void) { uint64_t v; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v)); return v; }
static void field(const char *name, uint64_t value) { platform_puts(name); platform_hex(value); }
/* A tick budget that scales with a delay injected at a kernel site (perturb.h), so a delay alone cannot exhaust it; a lost
 * wake still exceeds any scaled budget. One loaded turn costs a few instructions; a kernel call costs hundreds. */
static TickType_t budget(unsigned ms)
{
#ifdef SS_KERNEL_TEST_PERTURB
    const volatile uint32_t *table = (const volatile uint32_t *)(uintptr_t)SS_KERNEL_TEST_PERTURB;
    unsigned turns = 0;
    for (unsigned site = 0; site < SS_PERTURB_SITES; ++site) turns += table[1 + site];
    return pdMS_TO_TICKS(ms) * (1 + turns / 100);
#else
    return pdMS_TO_TICKS(ms);
#endif
}

/* ---- Phase 1 ------------------------------------------------------------------------------------------------- */
#define CANDIDATES 48U
#define HOLDS 40U
#define HOLD_BUDGET 3000000UL /* loop iterations of the holder before it gives up waiting for the others */
#define OPERATIONS_PER_HOLD 20U
extern unsigned ss_kernel_test_object_slot(const void *object) __attribute__((weak));
static StaticQueue_t candidate_state[CANDIDATES];
static uint8_t candidate_storage[CANDIDATES][sizeof(uint32_t)];
static QueueHandle_t candidate[CANDIDATES];
static QueueHandle_t slow_queue, own_queue[CORES];
static void *volatile slow_object;
static volatile unsigned phase1_stop, phase1_ready[CORES], hold_open;
static uint64_t progress[CORES] __attribute__((aligned(64)));
static uint64_t hold_start[CORES];
static volatile unsigned holds_taken, holds_expired;

/* Called inside the queue's critical section by every successful send (traceQUEUE_SEND). While armed, the send to the
 * slow queue keeps its lock until every other core has made progress or the budget is spent. */
void ss_object_hook(void *queue)
{
    if (queue != slow_object || !__atomic_load_n(&hold_open, __ATOMIC_ACQUIRE)) return;
    for (unsigned core = 2; core < CORES; ++core) hold_start[core] = __atomic_load_n(&progress[core], __ATOMIC_ACQUIRE);
    for (unsigned long spin = 0; spin < HOLD_BUDGET; ++spin) {
        unsigned ready = 0;
        for (unsigned core = 2; core < CORES; ++core)
            ready += __atomic_load_n(&progress[core], __ATOMIC_ACQUIRE) >= hold_start[core] + OPERATIONS_PER_HOLD;
        if (ready == CORES - 2) { ++holds_taken; return; }
    }
    ++holds_taken; ++holds_expired;
}
static void holder_task(void *unused)
{
    (void)unused;
    uint32_t value = 0;
    __atomic_store_n(&phase1_ready[1], 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&hold_open, __ATOMIC_ACQUIRE)) vTaskDelay(1);
    for (unsigned i = 0; i < HOLDS; ++i) {
        configASSERT(xQueueSend(slow_queue, &value, pdMS_TO_TICKS(100)) == pdPASS);
        configASSERT(xQueueReceive(slow_queue, &value, pdMS_TO_TICKS(100)) == pdPASS);
        ++value;
    }
    __atomic_store_n(&phase1_stop, 1, __ATOMIC_RELEASE);
    vTaskSuspend(NULL);
}
static void independent_task(void *argument)
{
    const unsigned core = (unsigned)(uintptr_t)argument;
    uint32_t value = 0;
    __atomic_store_n(&phase1_ready[core], 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&phase1_stop, __ATOMIC_ACQUIRE)) {
        configASSERT(xQueueSend(own_queue[core], &value, 0) == pdPASS);
        configASSERT(xQueueReceive(own_queue[core], &value, 0) == pdPASS);
        __atomic_fetch_add(&progress[core], 1, __ATOMIC_RELEASE);
        ++value;
    }
    vTaskSuspend(NULL);
}

/* ---- Phase 2 ------------------------------------------------------------------------------------------------- */
#define SOURCES 16U            /* 0 to 7: tasks (1 to 3 produce); 8 to 15: the interrupt of core source - 8 */
#define PER_SOURCE 4096U
#define PRODUCED 400U
struct message { uint32_t source, sequence; };
static StaticQueue_t shared_state;
static uint8_t shared_storage[8 * sizeof(struct message)];
static QueueHandle_t shared;
static uint32_t produced_ok[SOURCES], received[SOURCES];
static uint32_t seen[SOURCES][PER_SOURCE / 32];
static volatile unsigned producers_done, consumers_done, workers_done, isr_stop, stream_done;
static uint32_t isr_sequence[CORES];
static uint32_t last_seen[2][SOURCES];

static StaticSemaphore_t mutex_state, count_state;
static SemaphoreHandle_t mutex, counting;
static volatile uint32_t in_critical, mutex_entries, inherit_waits;
static StaticEventGroup_t group_state;
static EventGroupHandle_t group;
static volatile uint32_t group_successes, group_sets;
static StaticStreamBuffer_t stream_state;
static uint8_t stream_storage[257];
static StreamBufferHandle_t stream;
static volatile uint32_t stream_sent, stream_received, stream_bad;
static volatile uint32_t isr_gives, task_takes;

void ss_wake_isr(unsigned core)
{
    BaseType_t woken = pdFALSE;
    /* A delayed run takes more interrupts before the producers finish; a source sends at most PER_SOURCE messages. */
    if (shared && !isr_stop && isr_sequence[core] < PER_SOURCE) {
        struct message m = { SOURCES / 2 + core, isr_sequence[core] };
        if (xQueueSendFromISR(shared, &m, &woken) == pdPASS) {
            __atomic_fetch_add(&produced_ok[m.source], 1, __ATOMIC_RELAXED);
            ++isr_sequence[core];
        }
        if (counting && xSemaphoreGiveFromISR(counting, &woken) == pdPASS) __atomic_fetch_add(&isr_gives, 1, __ATOMIC_RELAXED);
    }
    portYIELD_FROM_ISR(woken);
}
static void produce(void *argument)
{
    const unsigned core = (unsigned)(uintptr_t)argument;
    uint32_t sequence = 0;
    for (unsigned i = 0; i < PRODUCED; ++i) {
        struct message m = { core, sequence };
        if (xQueueSend(shared, &m, 2) == pdPASS) { ++sequence; __atomic_fetch_add(&produced_ok[core], 1, __ATOMIC_RELAXED); }
        if ((i & 7) == 0) vTaskDelay(1);
    }
    __atomic_fetch_add(&producers_done, 1, __ATOMIC_ACQ_REL);
    vTaskSuspend(NULL);
}
static void consume(void *argument)
{
    const unsigned id = (unsigned)(uintptr_t)argument;
    for (unsigned i = 0; i < SOURCES; ++i) last_seen[id][i] = UINT32_MAX;
    unsigned quiet = 0;
    for (;;) {
        struct message m;
        if (xQueueReceive(shared, &m, 1) == pdPASS) {
            quiet = 0;
            configASSERT(m.source < SOURCES && m.sequence < PER_SOURCE);
            const uint32_t bit = UINT32_C(1) << (m.sequence % 32);
            configASSERT(!(__atomic_fetch_or(&seen[m.source][m.sequence / 32], bit, __ATOMIC_ACQ_REL) & bit)); /* no duplicate */
            configASSERT(last_seen[id][m.source] == UINT32_MAX || m.sequence > last_seen[id][m.source]);      /* per-source order */
            last_seen[id][m.source] = m.sequence;
            __atomic_fetch_add(&received[m.source], 1, __ATOMIC_RELAXED);
        } else if (__atomic_load_n(&producers_done, __ATOMIC_ACQUIRE) == 3 && __atomic_load_n(&isr_stop, __ATOMIC_ACQUIRE) &&
                   uxQueueMessagesWaiting(shared) == 0 && ++quiet > 3) break;
    }
    __atomic_fetch_add(&consumers_done, 1, __ATOMIC_ACQ_REL);
    vTaskSuspend(NULL);
}
/* Contend for the mutex from several cores with different priorities; the holder keeps it across a delay so that a
 * higher-priority waiter makes it inherit. */
static void mutex_task(void *argument)
{
    const unsigned id = (unsigned)(uintptr_t)argument;
    for (unsigned i = 0; i < 120; ++i) {
        if (xSemaphoreTake(mutex, id & 1 ? 1 : 3) == pdPASS) {
            configASSERT(__atomic_fetch_add(&in_critical, 1, __ATOMIC_ACQ_REL) == 0);
            ++mutex_entries;
            if (id == 0) vTaskDelay(1);
            else for (volatile unsigned spin = 0; spin < 40; ++spin) {}
            configASSERT(__atomic_fetch_sub(&in_critical, 1, __ATOMIC_ACQ_REL) == 1);
            configASSERT(xSemaphoreGive(mutex) == pdPASS);
        } else __atomic_fetch_add(&inherit_waits, 1, __ATOMIC_RELAXED);
        vTaskDelay(1);
    }
    __atomic_fetch_add(&workers_done, 1, __ATOMIC_ACQ_REL);
    vTaskSuspend(NULL);
}
static void group_setter(void *unused)
{
    (void)unused;
    for (unsigned i = 0; i < 200; ++i) {
        (void)xEventGroupSetBits(group, 1U << (i % 4));
        __atomic_fetch_add(&group_sets, 1, __ATOMIC_RELAXED);
        vTaskDelay(1);
    }
    __atomic_fetch_add(&workers_done, 1, __ATOMIC_ACQ_REL);
    vTaskSuspend(NULL);
}
static void group_waiter(void *argument)
{
    const uint32_t want = 1U << ((unsigned)(uintptr_t)argument % 4);
    for (unsigned i = 0; i < 120; ++i) {
        const EventBits_t bits = xEventGroupWaitBits(group, want, pdTRUE, pdFALSE, 2);
        if (bits & want) __atomic_fetch_add(&group_successes, 1, __ATOMIC_RELAXED);
    }
    __atomic_fetch_add(&workers_done, 1, __ATOMIC_ACQ_REL);
    vTaskSuspend(NULL);
}
static void stream_sender(void *unused)
{
    (void)unused;
    uint8_t byte = 0;
    for (unsigned i = 0; i < 3000; ++i) {
        if (xStreamBufferSend(stream, &byte, 1, 1) == 1) { ++byte; ++stream_sent; }
    }
    __atomic_store_n(&stream_done, 1, __ATOMIC_RELEASE);
    __atomic_fetch_add(&workers_done, 1, __ATOMIC_ACQ_REL);
    vTaskSuspend(NULL);
}
static void stream_receiver(void *unused)
{
    (void)unused;
    uint8_t expected = 0, byte;
    unsigned quiet = 0;
    while (quiet < 50) {
        if (xStreamBufferReceive(stream, &byte, 1, 1) == 1) {
            quiet = 0;
            if (byte != expected) ++stream_bad;
            ++expected; ++stream_received;
        } else if (__atomic_load_n(&stream_done, __ATOMIC_ACQUIRE) && xStreamBufferIsEmpty(stream)) ++quiet;
    }
    __atomic_fetch_add(&workers_done, 1, __ATOMIC_ACQ_REL);
    vTaskSuspend(NULL);
}
static void semaphore_taker(void *unused)
{
    (void)unused;
    for (unsigned i = 0; i < 400; ++i)
        if (xSemaphoreTake(counting, 1) == pdPASS) __atomic_fetch_add(&task_takes, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&workers_done, 1, __ATOMIC_ACQ_REL);
    vTaskSuspend(NULL);
}

/* ---- Control ------------------------------------------------------------------------------------------------- */
#define STACK 2048U
static StaticTask_t task_state[24], control_state;
static StackType_t task_stack[24][STACK], control_stack[4096];
static unsigned task_next;
static void spawn(TaskFunction_t function, const char *name, uintptr_t argument, UBaseType_t priority, unsigned core)
{
    configASSERT(task_next < 24);
    configASSERT(xTaskCreateStaticAffinitySet(function, name, STACK, (void *)argument, priority, task_stack[task_next],
                                              &task_state[task_next], 1U << core));
    ++task_next;
}
static void wait_for(volatile unsigned *value, unsigned target)
{
    const TickType_t start = xTaskGetTickCount();
    while (__atomic_load_n(value, __ATOMIC_ACQUIRE) < target) {
        configASSERT(xTaskGetTickCount() - start < budget(4000));
        vTaskDelay(1);
    }
}
static void phase1(void)
{
    for (unsigned i = 0; i < CANDIDATES; ++i) {
        candidate[i] = xQueueCreateStatic(1, sizeof(uint32_t), candidate_storage[i], &candidate_state[i]);
        configASSERT(candidate[i]);
    }
    slow_queue = candidate[0];
    unsigned used = 1;
    for (unsigned core = 2; core < CORES; ++core) {
        for (unsigned i = used; i < CANDIDATES; ++i) {
            bool distinct = !ss_kernel_test_object_slot || ss_kernel_test_object_slot(candidate[i]) != ss_kernel_test_object_slot(slow_queue);
            for (unsigned other = 2; other < core && distinct; ++other)
                distinct = !ss_kernel_test_object_slot || ss_kernel_test_object_slot(candidate[i]) != ss_kernel_test_object_slot(own_queue[other]);
            if (distinct) { own_queue[core] = candidate[i]; used = i + 1; break; }
        }
        configASSERT(own_queue[core]);
    }
    field("phase1 object-slots=", ss_kernel_test_object_slot != 0);
    if (ss_kernel_test_object_slot) {
        field(" holder=", ss_kernel_test_object_slot(slow_queue));
        for (unsigned core = 2; core < CORES; ++core) field(" other=", ss_kernel_test_object_slot(own_queue[core]));
    }
    platform_puts("\n");
    slow_object = slow_queue;
    spawn(holder_task, "holder", 0, 3, 1);
    for (unsigned core = 2; core < CORES; ++core) spawn(independent_task, "independent", core, 3, core);
    for (unsigned core = 1; core < CORES; ++core) wait_for(&phase1_ready[core], 1);
    for (unsigned core = 2; core < CORES; ++core) {
        const TickType_t start = xTaskGetTickCount();
        while (__atomic_load_n(&progress[core], __ATOMIC_ACQUIRE) < 2) {
            configASSERT(xTaskGetTickCount() - start < budget(4000));
            vTaskDelay(1);
        }
    }
    __atomic_store_n(&hold_open, 1, __ATOMIC_RELEASE);
    wait_for(&phase1_stop, 1);
    __atomic_store_n(&hold_open, 0, __ATOMIC_RELEASE);
    vTaskDelay(2);
    field("phase1 holds=", holds_taken); field(" expired=", holds_expired);
    for (unsigned core = 2; core < CORES; ++core) { field(" ops-core", core); field("=", progress[core]); }
    platform_puts("\n");
    configASSERT(holds_taken == HOLDS);
    if (holds_expired) {
        platform_puts("FAIL objects: operations on independent objects did not complete while an object lock was held\n");
        platform_exit(1);
    }
}
static void phase2(void)
{
    shared = xQueueCreateStatic(8, sizeof(struct message), shared_storage, &shared_state);
    mutex = xSemaphoreCreateMutexStatic(&mutex_state);
    counting = xSemaphoreCreateCountingStatic(1000, 0, &count_state);
    group = xEventGroupCreateStatic(&group_state);
    stream = xStreamBufferCreateStatic(sizeof(stream_storage) - 1, 1, stream_storage, &stream_state);
    configASSERT(shared && mutex && counting && group && stream);
    ss_wake_period = frequency() / 8000; /* each core takes an interrupt-level sender about every 125 us */
    for (unsigned core = 1; core < CORES; ++core) portYIELD_CORE(core); /* a core arms its timer on its first interrupt */
    for (unsigned core = 1; core <= 3; ++core) spawn(produce, "produce", core, 3, core);
    spawn(consume, "consume", 0, 3, 4);
    spawn(consume, "consume", 1, 3, 5);
    spawn(mutex_task, "mutexA", 0, 2, 2);
    spawn(mutex_task, "mutexB", 1, 4, 3);
    spawn(mutex_task, "mutexC", 2, 3, 6);
    spawn(mutex_task, "mutexD", 3, 4, 7);
    spawn(group_setter, "setter", 0, 3, 0);
    spawn(group_waiter, "waiter", 0, 3, 4);
    spawn(group_waiter, "waiter", 1, 3, 5);
    spawn(stream_sender, "sender", 0, 3, 6);
    spawn(stream_receiver, "receiver", 0, 3, 7);
    spawn(semaphore_taker, "taker", 0, 3, 2);
    wait_for(&producers_done, 3);
    isr_stop = 1;
    ss_wake_stop = 1;
    wait_for(&consumers_done, 2);
    wait_for(&workers_done, 10);
    uint32_t sent = 0, got = 0;
    for (unsigned i = 0; i < SOURCES; ++i) { sent += produced_ok[i]; got += received[i]; configASSERT(received[i] == produced_ok[i]); }
    field("phase2 messages sent=", sent); field(" received=", got); field(" mutex-entries=", mutex_entries);
    field(" mutex-timeouts=", inherit_waits); field(" group-sets=", group_sets); field(" group-successes=", group_successes);
    field(" stream-sent=", stream_sent); field(" stream-received=", stream_received); field(" stream-bad=", stream_bad);
    field(" isr-gives=", isr_gives); field(" task-takes=", task_takes); platform_puts("\n");
    configASSERT(sent == got && sent > 3 * PRODUCED / 2);
    configASSERT(in_critical == 0 && mutex_entries > 100);
    configASSERT(stream_bad == 0 && stream_sent == stream_received && stream_sent > 100);
    configASSERT(group_successes > 20);
    configASSERT(task_takes > 10 && task_takes <= isr_gives);
}
static void control(void *unused)
{
    (void)unused;
    phase1();
    platform_puts("phase1 passed\n");
    phase2();
    platform_puts("PASS objects: independent objects do not wait for each other, and queues, semaphores, a mutex with inheritance, an event group and a stream buffer lose, duplicate and reorder nothing across eight cores with interrupt-level senders\n");
    platform_exit(0);
}
void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
    (void)task; platform_panic(name, 0);
}
void kernel_main(void)
{
    platform_init();
    ss_wake_period = 0; /* the control task arms the interrupt senders when phase 2 starts */
    configASSERT(xTaskCreateStaticAffinitySet(control, "control", 4096, 0, 5, control_stack, &control_state, 1));
    vTaskStartScheduler();
    platform_panic("scheduler returned", 0);
}
