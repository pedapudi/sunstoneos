#include "FreeRTOS.h"
#include "task.h"
#include "sunstone_latency.h"
#include "latency_cause.h"
#include "cause-ping.h"
#include <stdatomic.h>
#include <stddef.h>

#ifndef SUNSTONE_LATENCY_CAUSE
#error "The ping diagnostic requires the cause profile's free-running EL0 cycle counter"
#endif

/* Only this normal-BSS cacheline moves between the two active participants.
 * Reports, admission, readiness and completion live on other cachelines. */
static struct {
    _Alignas(64) _Atomic uint64_t value;
    unsigned char padding[56];
} cause_ping_turn;
_Static_assert(sizeof(cause_ping_turn) == 64, "one normal-WB turn cacheline");
static struct {
    _Alignas(64) _Atomic unsigned started, abort, done;
    uint64_t deadline;
} control;
static struct {
    _Alignas(64) _Atomic unsigned ready, done, status;
} peer_control[LATENCY_CAUSE_PING_PAIRS];
static struct latency_cause_ping_report result;
static TaskHandle_t tasks[3];
static StaticTask_t task_state[3];
static StackType_t task_stack[3][1024];
static const unsigned task_core[3] = {0, 1, 6};
#define FINISH_TURN UINT64_MAX
#define ABORT_TURN (UINT64_MAX - 1)
#define TIMEOUT_POLL_MASK 63u

#ifdef SS_LATENCY_CAUSE_PING_TEST
extern uint64_t ss_latency_cause_ping_test_now(void);
extern uint64_t ss_latency_cause_ping_test_cycles(void);
static uint64_t now(void) { return ss_latency_cause_ping_test_now(); }
static uint64_t cycles(void) { return ss_latency_cause_ping_test_cycles(); }
#else
/* A relaxed counter read is sufficient for the coarse one-second timeout. It
 * adds no ISB inside the polling loop; each measured sample has exactly the two
 * ISB/PMCCNTR reads below. Timeout polling occurs every 64 failed turn loads. */
static uint64_t now(void)
{
    uint64_t value;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(value) :: "memory");
    return value;
}
static uint64_t cycles(void)
{
    uint64_t value;
    __asm__ volatile("isb\n mrs %0, pmccntr_el0" : "=r"(value) :: "memory");
    return value;
}
#endif

static uint64_t group(unsigned core)
{
    /* Cached startup validates the six-A55/two-A76 placement. Both groups
     * share the DSU; this field does not claim a distinct interconnect hop. */
    return core < 6 ? LATENCY_CAUSE_PING_CPU_L : LATENCY_CAUSE_PING_CPU_B;
}
static unsigned stopped(void)
{ return atomic_load_explicit(&control.abort, memory_order_acquire); }
static unsigned wait_turn(uint64_t expected, uint64_t deadline)
{
    unsigned polls = 0;
    for (;;) {
        uint64_t value = atomic_load_explicit(&cause_ping_turn.value, memory_order_acquire);
        if (value == expected) return LATENCY_CAUSE_PING_OK;
        if (value == ABORT_TURN) return LATENCY_CAUSE_PING_ABORTED;
        if (value > expected) return LATENCY_CAUSE_PING_SEQUENCE;
        if (!(++polls & TIMEOUT_POLL_MASK) && now() >= deadline)
            return LATENCY_CAUSE_PING_TIMEOUT;
    }
}
static unsigned wait_counter(unsigned core, uint64_t deadline)
{
    while (!ss_latency_cause_core_ready(core)) {
        if (stopped()) return LATENCY_CAUSE_PING_ABORTED;
        if (now() >= deadline) return LATENCY_CAUSE_PING_NOT_READY;
        vTaskDelay(1); /* Admit the IRQ that configures this CPU's PMU. */
    }
    return LATENCY_CAUSE_PING_OK;
}
static void park(unsigned index)
{
    atomic_fetch_or_explicit(&control.done, 1u << index, memory_order_release);
    vTaskSuspend(NULL);
    configASSERT(0);
}
static void peer(void *argument)
{
    unsigned index = (unsigned)(uintptr_t)argument, pair = index - 1;
    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    unsigned actual = (unsigned)portGET_CORE_ID();
    struct latency_cause_ping_pair *row = &result.pair[pair];
    row->target_core = actual;
    row->target_group = actual < 8 ? group(actual) : UINT64_MAX;
    unsigned status = actual == task_core[index] ? LATENCY_CAUSE_PING_OK : LATENCY_CAUSE_PING_WRONG_CORE;
    uint64_t deadline = control.deadline;
    if (status == LATENCY_CAUSE_PING_OK)
        status = stopped() ? LATENCY_CAUSE_PING_ABORTED : wait_counter(actual, deadline);
    if (status == LATENCY_CAUSE_PING_OK) {
        row->target_irq_begin = ss_latency_cause_irq_count(actual);
        atomic_store_explicit(&peer_control[pair].ready, 1, memory_order_release);
        for (unsigned i = 0; i < LATENCY_CAUSE_PING_SAMPLES; ++i) {
            status = wait_turn(2u * i + 1, deadline);
            if (status != LATENCY_CAUSE_PING_OK) break;
            atomic_store_explicit(&cause_ping_turn.value, 2u * i + 2, memory_order_release);
        }
        /* Keep all completion stores after CPU0's final measured timestamp. */
        if (status == LATENCY_CAUSE_PING_OK) status = wait_turn(FINISH_TURN, deadline);
        row->target_irq_end = ss_latency_cause_irq_count(actual);
    }
    atomic_store_explicit(&peer_control[pair].status, status, memory_order_relaxed);
    atomic_store_explicit(&peer_control[pair].done, 1, memory_order_release);
    if (status != LATENCY_CAUSE_PING_OK)
        atomic_store_explicit(&cause_ping_turn.value, ABORT_TURN, memory_order_release);
    park(index);
}
static unsigned reference(unsigned core, uint64_t deadline)
{
    struct latency_cause_ping_reference *row = &result.reference;
    row->core = core;
    row->group = group(core);
    row->begin_tick = now();
    row->irq_begin = ss_latency_cause_irq_count(core);
    unsigned status = LATENCY_CAUSE_PING_OK;
    for (unsigned i = 0; i < LATENCY_CAUSE_PING_SAMPLES; ++i) {
        if (now() >= deadline) { status = LATENCY_CAUSE_PING_TIMEOUT; break; }
        uint64_t begin = cycles();
        atomic_store_explicit(&cause_ping_turn.value, i + 1, memory_order_release);
        status = wait_turn(i + 1, deadline);
        uint64_t end = cycles();
        if (status != LATENCY_CAUSE_PING_OK) break;
        if (end <= begin || end - begin > UINT32_MAX) { status = LATENCY_CAUSE_PING_CYCLE_RANGE; break; }
        row->cycles[i] = (uint32_t)(end - begin);
        ++row->completed;
    }
    row->irq_end = ss_latency_cause_irq_count(core);
    row->end_tick = now();
    row->status = status;
    return status;
}
static unsigned exchange(unsigned pair, unsigned core, uint64_t deadline)
{
    struct latency_cause_ping_pair *row = &result.pair[pair];
    row->source_core = core;
    row->source_group = group(core);
    atomic_store_explicit(&cause_ping_turn.value, 0, memory_order_release);
    xTaskNotifyGive(tasks[pair + 1]);
    while (!atomic_load_explicit(&peer_control[pair].ready, memory_order_acquire)) {
        if (atomic_load_explicit(&peer_control[pair].done, memory_order_acquire))
            return atomic_load_explicit(&peer_control[pair].status, memory_order_relaxed);
        if (stopped()) return LATENCY_CAUSE_PING_ABORTED;
        if (now() >= deadline) return LATENCY_CAUSE_PING_TIMEOUT;
        vTaskDelay(1);
    }
    if (atomic_load_explicit(&peer_control[pair].done, memory_order_acquire))
        return atomic_load_explicit(&peer_control[pair].status, memory_order_relaxed);
    row->begin_tick = now();
    row->source_irq_begin = ss_latency_cause_irq_count(core);
    unsigned status = LATENCY_CAUSE_PING_OK;
    for (unsigned i = 0; i < LATENCY_CAUSE_PING_SAMPLES; ++i) {
        if (now() >= deadline) { status = LATENCY_CAUSE_PING_TIMEOUT; break; }
        uint64_t begin = cycles();
        atomic_store_explicit(&cause_ping_turn.value, 2u * i + 1, memory_order_release);
        status = wait_turn(2u * i + 2, deadline);
        uint64_t end = cycles();
        if (status == LATENCY_CAUSE_PING_ABORTED &&
            atomic_load_explicit(&peer_control[pair].done, memory_order_acquire))
            status = atomic_load_explicit(&peer_control[pair].status, memory_order_relaxed);
        if (status != LATENCY_CAUSE_PING_OK) break;
        if (end <= begin || end - begin > UINT32_MAX) { status = LATENCY_CAUSE_PING_CYCLE_RANGE; break; }
        row->cycles[i] = (uint32_t)(end - begin);
        ++row->completed;
    }
    row->source_irq_end = ss_latency_cause_irq_count(core);
    row->end_tick = now();
    atomic_store_explicit(&cause_ping_turn.value,
                          status == LATENCY_CAUSE_PING_OK ? FINISH_TURN : ABORT_TURN,
                          memory_order_release);
    if (status == LATENCY_CAUSE_PING_OK) {
        while (!atomic_load_explicit(&peer_control[pair].done, memory_order_acquire)) {
            if (now() >= deadline) { status = LATENCY_CAUSE_PING_TIMEOUT; break; }
            vTaskDelay(1);
        }
        if (status == LATENCY_CAUSE_PING_OK)
            status = atomic_load_explicit(&peer_control[pair].status, memory_order_relaxed);
    }
    row->status = status;
    return status;
}
static void driver(void *argument)
{
    (void)argument;
    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    unsigned actual = (unsigned)portGET_CORE_ID();
    uint64_t deadline = control.deadline;
    unsigned status = actual == 0 ? LATENCY_CAUSE_PING_OK : LATENCY_CAUSE_PING_WRONG_CORE;
    result.reference.core = actual;
    result.reference.group = actual < 8 ? group(actual) : UINT64_MAX;
    if (status == LATENCY_CAUSE_PING_OK)
        status = stopped() ? LATENCY_CAUSE_PING_ABORTED : wait_counter(actual, deadline);
    if (status == LATENCY_CAUSE_PING_OK) status = reference(actual, deadline);
    for (unsigned pair = 0; status == LATENCY_CAUSE_PING_OK && pair < LATENCY_CAUSE_PING_PAIRS; ++pair)
        status = stopped() ? LATENCY_CAUSE_PING_ABORTED : exchange(pair, actual, deadline);
    if (status != LATENCY_CAUSE_PING_OK) {
        atomic_store_explicit(&control.abort, 1, memory_order_release);
        atomic_store_explicit(&cause_ping_turn.value, ABORT_TURN, memory_order_release);
        for (unsigned i = 1; i < 3; ++i) xTaskNotifyGive(tasks[i]);
    }
    result.status = status;
    result.end_tick = now();
    park(0);
}

void latency_cause_ping_init(void)
{
    configASSERT(!tasks[0]);
    result = (struct latency_cause_ping_report){ .magic = LATENCY_CAUSE_PING_MAGIC,
        .version = 1, .bytes = sizeof(result), .samples = LATENCY_CAUSE_PING_SAMPLES };
    for (unsigned pair = 0; pair < LATENCY_CAUSE_PING_PAIRS; ++pair) {
        result.pair[pair].source_core = result.pair[pair].target_core = UINT64_MAX;
        result.pair[pair].source_group = result.pair[pair].target_group = UINT64_MAX;
    }
    for (unsigned i = 0; i < 3; ++i) {
        tasks[i] = xTaskCreateStaticAffinitySet(i ? peer : driver, "cause-ping", 1024,
            (void *)(uintptr_t)i, configMAX_PRIORITIES - 1, task_stack[i], &task_state[i], 1u << task_core[i]);
        configASSERT(tasks[i]);
    }
}
bool latency_cause_ping_start(uint64_t frequency)
{
    uint64_t begin = now();
    if (!tasks[0] || !frequency || begin > UINT64_MAX - frequency) return false;
    unsigned expected = 0;
    if (!atomic_compare_exchange_strong_explicit(&control.started, &expected, 1,
                                                 memory_order_acq_rel, memory_order_acquire)) return false;
    result.frequency = frequency;
    result.begin_tick = begin;
    control.deadline = begin + frequency;
    xTaskNotifyGive(tasks[0]);
    return true;
}
bool latency_cause_ping_done(void)
{
    if (atomic_load_explicit(&control.done, memory_order_acquire) != 7) return false;
    for (unsigned i = 0; i < 3; ++i)
        if (!ss_latency_task_suspended(tasks[i])) return false;
    return true;
}
void latency_cause_ping_stop(void)
{
    if (!atomic_load_explicit(&control.started, memory_order_acquire) ||
        atomic_load_explicit(&control.done, memory_order_acquire) == 7) return;
    atomic_store_explicit(&control.abort, 1, memory_order_release);
    for (unsigned i = 0; i < 3; ++i) xTaskNotifyGive(tasks[i]);
}
bool latency_cause_ping_snapshot(struct latency_cause_ping_report *out)
{
    if (!out || !latency_cause_ping_done()) return false;
    *out = result;
    for (unsigned pair = 0; pair < LATENCY_CAUSE_PING_PAIRS; ++pair)
        if (out->pair[pair].status == LATENCY_CAUSE_PING_PENDING)
            out->pair[pair].status = atomic_load_explicit(&peer_control[pair].status, memory_order_acquire);
    return true;
}
