/* The real control recorder with modeled architectural boundaries. These
 * cases test stop ownership, publication and histogram arithmetic, not board
 * interrupt timing or electrical behavior. */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "FreeRTOS.h"
#include "task.h"
#include "../apps/latency/control.h"

struct control_test_task {
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    void (*entry)(void *);
    void *argument;
    unsigned core;
    _Atomic unsigned suspended, frame_ack;
    uint32_t pending;
};
static _Thread_local unsigned model_core, irq_masked;
static _Thread_local TaskHandle_t model_task;
static pthread_mutex_t irq_lock[8];
static TaskHandle_t model_tasks[8];
static unsigned model_task_count;
static _Atomic uint64_t model_counter[8], model_compare[8][2], model_enable[8][2];
static _Atomic unsigned model_sgis;
static _Atomic unsigned pause_after_copy, pause_after_notify, writer_paused, writer_resume;
static _Atomic unsigned nested_stop;
static unsigned nested_observed;
static unsigned native_irqs[32];

#define SUNSTONE_CONTROL_TEST 1
#ifndef SS_CONTROL_SOURCE
#define SS_CONTROL_SOURCE "../runtime/control_irq.c"
#endif
#include SS_CONTROL_SOURCE

unsigned ss_control_test_core(void) { return model_core; }
void ss_control_test_sgi(unsigned core)
{ atomic_fetch_or_explicit(&model_sgis, 1u << core, memory_order_release); }
uint64_t ss_control_test_counter(void)
{
    const uint64_t now = atomic_fetch_add_explicit(&model_counter[model_core], 1, memory_order_relaxed);
    if (atomic_exchange_explicit(&nested_stop, 0, memory_order_acquire)) {
        assert(model_core == 1 && irq_masked);
        /* Model the remote controller's stop publication while task PPI30
         * has passed its initial check, then the higher control PPI27. */
        vSunstoneControlStop();
        __wrap_vApplicationIRQHandler(INTID);
        assert(!__atomic_load_n(&state[1].isr_stopped, __ATOMIC_ACQUIRE));
        assert(state[1].task_irq_count == 0);
        ++nested_observed;
    }
    return now;
}
uint64_t ss_control_test_frequency(void) { return 24000000; }
void ss_control_test_program(unsigned path, uint64_t compare, uint64_t enable)
{
    if (!enable) assert(irq_masked);
    atomic_store_explicit(&model_compare[model_core][path], compare, memory_order_relaxed);
    atomic_store_explicit(&model_enable[model_core][path], enable, memory_order_relaxed);
}
uint64_t ss_control_test_irq_mask(void)
{ const unsigned old = irq_masked; irq_masked = 1; return old; }
void ss_control_test_irq_restore(uint64_t previous) { irq_masked = (unsigned)previous; }
UBaseType_t ss_control_test_mask(void)
{
    assert(!irq_masked);
    pthread_mutex_lock(&irq_lock[model_core]);
    irq_masked = 1;
    return 0;
}
void ss_control_test_unmask(UBaseType_t previous)
{
    assert(!previous && irq_masked);
    irq_masked = 0;
    pthread_mutex_unlock(&irq_lock[model_core]);
    if (atomic_exchange_explicit(&pause_after_copy, 0, memory_order_acquire)) {
        atomic_store_explicit(&writer_paused, 1, memory_order_release);
        while (!atomic_load_explicit(&writer_resume, memory_order_acquire)) sched_yield();
    }
}
void __real_vApplicationIRQHandler(uint64_t interrupt)
{ assert(interrupt < 32); ++native_irqs[interrupt]; }
static void *thread_entry(void *argument)
{
    model_task = argument;
    model_core = model_task->core;
    model_task->entry(model_task->argument);
    return NULL;
}
TaskHandle_t xTaskCreateStaticAffinitySet(void (*entry)(void *), const char *name,
    uint32_t depth, void *argument, UBaseType_t priority, StackType_t *stack,
    StaticTask_t *storage, UBaseType_t affinity)
{
    (void)name; (void)depth; (void)priority; (void)stack; (void)storage;
    assert(affinity && model_task_count < 8);
    TaskHandle_t task = calloc(1, sizeof(*task));
    assert(task);
    task->entry = entry; task->argument = argument;
    task->core = (unsigned)__builtin_ctzll(affinity);
    pthread_mutex_init(&task->mutex, NULL);
    pthread_cond_init(&task->changed, NULL);
    model_tasks[model_task_count++] = task;
    assert(!pthread_create(&task->thread, NULL, thread_entry, task));
    return task;
}
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t ticks)
{
    (void)ticks;
    assert(clear && model_task);
    pthread_mutex_lock(&model_task->mutex);
    while (!model_task->pending) pthread_cond_wait(&model_task->changed, &model_task->mutex);
    const uint32_t pending = model_task->pending;
    model_task->pending = 0;
    pthread_mutex_unlock(&model_task->mutex);
    if (atomic_exchange_explicit(&pause_after_notify, 0, memory_order_acquire)) {
        atomic_store_explicit(&writer_paused, 1, memory_order_release);
        while (!atomic_load_explicit(&writer_resume, memory_order_acquire)) sched_yield();
    }
    return pending;
}
BaseType_t xTaskNotifyGive(TaskHandle_t task)
{
    assert(task);
    pthread_mutex_lock(&task->mutex);
    ++task->pending;
    pthread_cond_signal(&task->changed);
    pthread_mutex_unlock(&task->mutex);
    return pdPASS;
}
void vTaskNotifyGiveFromISR(TaskHandle_t task, BaseType_t *woken)
{ (void)xTaskNotifyGive(task); *woken = pdTRUE; }
void vTaskDelay(TickType_t ticks)
{
    struct timespec interval = {0, (long)ticks * 1000000};
    nanosleep(&interval, NULL);
}
void vTaskSuspend(TaskHandle_t task)
{ (void)task; atomic_store_explicit(&model_task->suspended, 1, memory_order_release); pthread_exit(NULL); }
eTaskState eTaskGetState(TaskHandle_t task)
{ return atomic_load_explicit(&task->suspended, memory_order_acquire) ? eSuspended : eBlocked; }
#ifdef SUNSTONE_LATENCY
bool ss_latency_task_suspended(void *handle)
{
    TaskHandle_t task = handle;
    return atomic_load_explicit(&task->suspended, memory_order_acquire) &&
           atomic_load_explicit(&task->frame_ack, memory_order_acquire);
}
#endif

static void deliver(unsigned core, unsigned interrupt)
{
    const unsigned previous = model_core;
    pthread_mutex_lock(&irq_lock[core]);
    model_core = core; irq_masked = 1;
    __wrap_vApplicationIRQHandler(interrupt);
    irq_masked = 0; model_core = previous;
    pthread_mutex_unlock(&irq_lock[core]);
}
static void pending_sgis(void)
{
    unsigned bits = atomic_exchange_explicit(&model_sgis, 0, memory_order_acquire);
    while (bits) {
        const unsigned c = (unsigned)__builtin_ctz(bits);
        bits &= bits - 1;
        deliver(c, 0);
    }
}
static uint64_t wall_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}
static void wait_count(unsigned core, unsigned path, uint64_t count)
{
    const uint64_t until = wall_ns() + 2000000000u;
    unsigned spins = 0;
    while (__atomic_load_n(&state[core].published[path], __ATOMIC_ACQUIRE) < count) {
        if (!(++spins & 1023u)) { assert(wall_ns() < until); sched_yield(); }
    }
}
static void wait_quiescent(void)
{
    const uint64_t until = wall_ns() + 2000000000u;
    while (!ulSunstoneControlQuiescent()) {
        assert(wall_ns() < until);
        pending_sgis();
        sched_yield();
    }
}
static void wait_paused(void)
{
    const uint64_t until = wall_ns() + 2000000000u;
    while (!atomic_load_explicit(&writer_paused, memory_order_acquire)) {
        assert(wall_ns() < until);
        sched_yield();
    }
}
static void finish_tasks(void)
{
#if ( INCLUDE_eTaskGetState == 1 )
    const uint64_t until = wall_ns() + 2000000000u;
    while (!ulSunstoneControlTasksSuspended()) { assert(wall_ns() < until); sched_yield(); }
#endif
    for (unsigned i = 0; i < model_task_count; ++i) {
        TaskHandle_t task = model_tasks[i];
        assert(!pthread_join(task->thread, NULL));
        pthread_cond_destroy(&task->changed);
        pthread_mutex_destroy(&task->mutex);
        free(task);
    }
}
static void fire(unsigned core, unsigned path, uint64_t late)
{
    const struct path *p = path ? &state[core].task : &state[core].control;
    atomic_store_explicit(&model_counter[core], p->deadline + late, memory_order_relaxed);
    deliver(core, path ? TASK_INTID : INTID);
}
static uint64_t histogram_count(const uint32_t *bins)
{
    uint64_t total = 0;
    for (unsigned b = 0; b < SS_CONTROL_BINS; ++b) total += bins[b];
    return total;
}
static void arithmetic(void)
{
    uint64_t served;
    unsigned missed;
    struct path p = {.period = 10, .deadline = 100};
    assert(advance(&p, 99, &served, &missed) == 0 && p.deadline == 100);
    assert(advance(&p, 100, &served, &missed) == 1 && served == 100 && p.deadline == 110 && !missed);
    assert(advance(&p, 145, &served, &missed) == 1 && served == 110 && p.deadline == 150 && missed == 3);
    p = (struct path){.period = 2400, .deadline = 100};
    const uint64_t now = 60 * UINT64_C(24000000);
    assert(advance(&p, now, &served, &missed) == 1 && p.deadline > now && p.deadline <= now + 2400);
    assert(missed == (now - 100) / 2400 && (p.deadline - 100) % 2400 == 0);
    p = (struct path){.period = 5, .deadline = UINT64_MAX - 2};
    assert(advance(&p, UINT64_MAX - 1, &served, &missed) == -1);
    p = (struct path){.deadline = 100};
    assert(advance(&p, 101, &served, &missed) == -1);
    __atomic_store_n(&frequency_hz, 24000000, __ATOMIC_RELEASE);
    assert(ticks_to_ns(1, 24000000) == 1000000000);
    assert(ticks_to_ns(1, 1440000000) == UINT32_MAX);
    assert(__atomic_load_n(&state[1].errors, __ATOMIC_RELAXED) & SS_CONTROL_ERROR_NS_RANGE);
    uint32_t bins[SS_CONTROL_BINS] = {9999, 1};
    assert(percentile_ticks(bins, 10000, 8) == 4);
    bins[1] = 2;
    assert(percentile_ticks(bins, 10001, 8) == 8);
    bins[0] = 9998; bins[1] = 1;
    assert(percentile_ticks(bins, 9999, 8) == 8);
    memset(bins, 0, sizeof(bins)); bins[SS_CONTROL_BINS - 1] = 10000;
    assert(percentile_ticks(bins, 10000, 2000) == 2000);
}
static void overflow_only(void)
{
    struct path p = {.period = 5, .deadline = UINT64_MAX - 2};
    uint64_t served;
    unsigned missed;
    assert(advance(&p, UINT64_MAX - 1, &served, &missed) == -1);
}
static void task_stop(void)
{
    const unsigned cores[] = {1}, rates[] = {1000}, paths[] = {2};
    assert(ulSunstoneControlStartPathsMode(cores, rates, paths, 1, SS_CONTROL_HISTOGRAM_ONLY) == 1);
    pending_sgis();
    fire(1, 1, 12);
    wait_count(1, 1, 1);
    const uint64_t before = state[1].task_irq_count;
    __atomic_store_n(&state[1].stop, 1, __ATOMIC_RELEASE);
    /* Dispatch can race stop publication after the wrapper's initial check.
     * The task-path handler itself must refuse this stale delivery. */
    model_core = 1; irq_masked = 1;
    atomic_store_explicit(&model_counter[1], state[1].task.deadline + 12, memory_order_relaxed);
    handle_task_path(&state[1]);
    irq_masked = 0; model_core = 0;
    assert(state[1].task_irq_count == before);
    assert(!atomic_load_explicit(&model_enable[1][0], memory_order_relaxed));
    assert(!atomic_load_explicit(&model_enable[1][1], memory_order_relaxed));
    vSunstoneControlStop(); wait_quiescent();
    struct ss_control_progress progress;
    assert(ulSunstoneControlProgressRead(&progress));
    assert(progress.quiescent_mask == 2 && progress.consumer_parked && progress.count[1][1] == 1);
    const unsigned native_before = native_irqs[TASK_INTID] + native_irqs[INTID];
    deliver(1, TASK_INTID); deliver(1, INTID);
    assert(native_irqs[TASK_INTID] + native_irqs[INTID] == native_before);
    assert(state[1].task_irq_count == before);
    finish_tasks();
}
static void unarmed_stop(void)
{
    const unsigned cores[] = {7}, rates[] = {10000}, paths[] = {1};
    assert(ulSunstoneControlStartPathsMode(cores, rates, paths, 1, SS_CONTROL_HISTOGRAM_ONLY) == 1);
    assert(model_task_count == 0);
    vSunstoneControlStop();
    deliver(7, INTID);
    wait_quiescent();
    assert(state[7].control.count == 0 && !native_irqs[INTID]);
    assert(!ulSunstoneControlStartPathsMode(cores, rates, paths, 1, SS_CONTROL_HISTOGRAM_ONLY));
}
static void last_task_write(void)
{
    const unsigned cores[] = {1}, rates[] = {1000}, paths[] = {2};
    assert(ulSunstoneControlStartPathsMode(cores, rates, paths, 1, SS_CONTROL_HISTOGRAM_ONLY) == 1);
    pending_sgis();
    atomic_store_explicit(&pause_after_copy, 1, memory_order_release);
    fire(1, 1, 12); wait_paused();
    vSunstoneControlStop(); pending_sgis();
    struct ss_control_progress progress;
    assert(ulSunstoneControlProgressRead(&progress));
    assert(__atomic_load_n(&state[1].isr_stopped, __ATOMIC_ACQUIRE));
    assert(!progress.quiescent_mask && !ulSunstoneControlQuiescent());
    atomic_store_explicit(&writer_resume, 1, memory_order_release);
    wait_quiescent();
    assert(state[1].task.count == 1 && histogram_count(state[1].task.latency_bin) == 1);
    deliver(1, TASK_INTID);
    assert(state[1].task.count == 1);
    finish_tasks();
}
static void nested_control_stop(void)
{
    const unsigned cores[] = {1}, rates[] = {1000}, paths[] = {3};
    assert(ulSunstoneControlStartPathsMode(cores, rates, paths, 1, SS_CONTROL_HISTOGRAM_ONLY) == 1);
    pending_sgis();
    atomic_store_explicit(&nested_stop, 1, memory_order_release);
    fire(1, 1, 12);
    assert(nested_observed == 1 && state[1].task_irq_count == 1);
    assert(__atomic_load_n(&state[1].isr_stopped, __ATOMIC_ACQUIRE));
    assert(!atomic_load_explicit(&model_enable[1][0], memory_order_relaxed));
    assert(!atomic_load_explicit(&model_enable[1][1], memory_order_relaxed));
    pending_sgis(); wait_quiescent();
    deliver(1, TASK_INTID); deliver(1, INTID);
    assert(state[1].task_irq_count == 1 && !state[1].control.count);
    finish_tasks();
}
static void coalesced_notification(void)
{
    const unsigned cores[] = {1}, rates[] = {1000}, paths[] = {2};
    assert(ulSunstoneControlStartPathsMode(cores, rates, paths, 1, SS_CONTROL_HISTOGRAM_ONLY) == 1);
    pending_sgis();
    atomic_store_explicit(&pause_after_notify, 1, memory_order_release);
    fire(1, 1, 12); wait_paused();
    fire(1, 1, 12);
    atomic_store_explicit(&writer_resume, 1, memory_order_release);
    const uint64_t until = wall_ns() + 2000000000u;
    while (!__atomic_load_n(&state[1].errors, __ATOMIC_RELAXED)) { assert(wall_ns() < until); sched_yield(); }
    vSunstoneControlStop(); pending_sgis(); wait_quiescent();
    assert(state[1].errors & SS_CONTROL_ERROR_NOTIFICATION);
    assert(!state[1].task.count);
    finish_tasks();
}
static void consumer_stop(void)
{
    const unsigned cores[] = {1}, rates[] = {1000}, paths[] = {1};
    assert(ulSunstoneControlStartPaths(cores, rates, paths, 1) == 1);
    pending_sgis();
    for (unsigned n = 0; n < 64; ++n) fire(1, 0, 8);
    vSunstoneControlStop(); pending_sgis(); wait_quiescent();
    assert(state[1].control.count == 64 && state[1].consumed == 64);
    assert(!state[1].gaps && !state[1].ring.dropped);
    assert(histogram_count(state[1].control.latency_bin) == 64);
    assert(histogram_count(state[1].control.jitter_bin) == 63);
    finish_tasks();
}
static void start_validation(void)
{
    const unsigned cores[] = {7}, rates[] = {1000}, zero_rate[] = {0}, excessive_rate[] = {24000001}, empty_paths[] = {0};
    assert(!ulSunstoneControlStartPaths(cores, rates, empty_paths, 1));
    assert(!ulSunstoneControlStart(cores, zero_rate, 1));
    assert(!ulSunstoneControlStart(cores, excessive_rate, 1));
    assert(!model_task_count);
    assert(ulSunstoneControlStart(cores, rates, 1) == 1 && state[7].paths == 3);
    pending_sgis(); vSunstoneControlStop(); pending_sgis(); wait_quiescent(); finish_tasks();
}
static void unselected_stale(void)
{
    const unsigned cores[] = {1}, rates[] = {1000}, paths[] = {2};
    assert(ulSunstoneControlStartPathsMode(cores, rates, paths, 1, SS_CONTROL_HISTOGRAM_ONLY) == 1);
    deliver(1, INTID);
    assert(!atomic_load_explicit(&model_enable[1][0], memory_order_relaxed));
    atomic_store_explicit(&model_enable[1][0], 1, memory_order_relaxed);
    deliver(1, INTID);
    assert(!atomic_load_explicit(&model_enable[1][0], memory_order_relaxed));
    assert(!state[1].control.count && !native_irqs[INTID]);
    vSunstoneControlStop(); pending_sgis(); wait_quiescent(); finish_tasks();
}
static void qualification(unsigned mode)
{
    assert(latency_control_start(mode));
    pending_sgis();
    const uint64_t until = wall_ns() + 2000000000u;
    while (!latency_control_ready()) { assert(wall_ns() < until); sched_yield(); }
    assert(latency_control_complete() == 0);
    struct latency_control_report report;
    assert(!latency_control_snapshot(&report));
    for (unsigned n = 0; n < 10000; ++n) {
        for (unsigned c = 1; c < 8; ++c) {
            fire(c, 0, 8);
            if (mode == LATENCY_CONTROL_BUSY) {
                fire(c, 1, 16);
                wait_count(c, 1, n + 1);
            }
        }
    }
    assert(latency_control_complete() == 0); /* 10,000 entries supply 9,999 jitter intervals. */
    for (unsigned c = 1; c < 8; ++c) fire(c, 0, 8);
    assert(latency_control_complete() == 1);
    latency_control_stop(); pending_sgis(); wait_quiescent();
    assert(latency_control_stopped() && latency_control_snapshot(&report));
    assert(report.magic == LATENCY_CONTROL_MAGIC && report.requested_mask == 0xfe &&
        report.ready_mask == 0xfe && report.quiescent_mask == 0xfe && report.complete_mask == 0xfe &&
        report.consumer_parked && !report.errors);
    for (unsigned i = 0; i < 7; ++i) {
        const struct latency_control_core *row = &report.core[i];
        assert(row->core == i + 1 && row->published_control == 10001 && !row->error_flags);
        assert(histogram_count(row->histogram[LATENCY_CONTROL_IRQ_JITTER]) == 10000);
        assert(row->stats[0].count == 10001 && !row->stats[0].missed);
        if (mode == LATENCY_CONTROL_BUSY) {
            assert(row->published_task == 10000 && row->stats[1].count == 10000);
            assert(histogram_count(row->histogram[LATENCY_CONTROL_TASK_LATENCY]) == 10000);
            assert(histogram_count(row->histogram[LATENCY_CONTROL_TASK_GAP]) == 10000);
        } else {
            assert(row->published_task == 0 && row->stats[1].count == 0);
            assert(histogram_count(row->histogram[LATENCY_CONTROL_TASK_LATENCY]) == 0);
        }
    }
    finish_tasks();
}
static void suspension_frame_ack(void)
{
    struct control_test_task consumer = {0}, path = {0};
    atomic_store_explicit(&consumer.suspended, 1, memory_order_release);
    atomic_store_explicit(&path.suspended, 1, memory_order_release);
    consumer_handle = &consumer;
    frequency_hz = 24000000;
    stop_requested = consumer_parked = 1;
    requested_mask = 1u << 1;
    state[1].requested = state[1].isr_stopped = state[1].task_parked = 1;
    state[1].paths = 3;
    state[1].task_handle = &path;
    assert(ulSunstoneControlQuiescent());
    assert(eTaskGetState(&consumer) == eSuspended && eTaskGetState(&path) == eSuspended);
    assert(!ulSunstoneControlTasksSuspended());
    atomic_store_explicit(&consumer.frame_ack, 1, memory_order_release);
    assert(!ulSunstoneControlTasksSuspended());
    atomic_store_explicit(&path.frame_ack, 1, memory_order_release);
    assert(ulSunstoneControlTasksSuspended());
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    for (unsigned c = 0; c < 8; ++c) pthread_mutex_init(&irq_lock[c], NULL);
    if (!strcmp(argv[1], "arithmetic")) arithmetic();
    else if (!strcmp(argv[1], "overflow-only")) overflow_only();
    else if (!strcmp(argv[1], "task-stop")) task_stop();
    else if (!strcmp(argv[1], "unarmed-stop")) unarmed_stop();
    else if (!strcmp(argv[1], "last-task-write")) last_task_write();
    else if (!strcmp(argv[1], "nested-control-stop")) nested_control_stop();
    else if (!strcmp(argv[1], "coalesced-notification")) coalesced_notification();
    else if (!strcmp(argv[1], "consumer-stop")) consumer_stop();
    else if (!strcmp(argv[1], "start-validation")) start_validation();
    else if (!strcmp(argv[1], "unselected-stale")) unselected_stale();
    else if (!strcmp(argv[1], "empty-stop")) { vSunstoneControlStop(); assert(ulSunstoneControlQuiescent()); }
    else if (!strcmp(argv[1], "suspended-query-disabled")) assert(!ulSunstoneControlTasksSuspended());
    else if (!strcmp(argv[1], "suspension-frame-ack")) suspension_frame_ack();
    else if (!strcmp(argv[1], "qualification-idle")) qualification(LATENCY_CONTROL_IDLE);
    else if (!strcmp(argv[1], "qualification-busy")) qualification(LATENCY_CONTROL_BUSY);
    else assert(0);
    puts("PASS control stop and qualification model");
    return 0;
}
