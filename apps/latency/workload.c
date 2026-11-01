#include "FreeRTOS.h"
#include "task.h"
#include "workload.h"
#include <stdatomic.h>
#ifdef SUNSTONE_LATENCY_MODEL
#include "platform.h"
#endif

_Static_assert(configNUMBER_OF_CORES == LATENCY_CORES, "requires eight cores");
enum phase { QUIET, IDLE, BUSY, STOP };
struct wake_slot {
    _Alignas(64) atomic_uint pending, completed, parked;
    uint64_t released;
    unsigned metric;
};
struct load_slot {
    _Alignas(64) atomic_uint ready, stopped;
    atomic_uint_fast64_t sweeps;
    uint64_t digest;
};
static struct wake_slot wake[LATENCY_CORES];
static struct load_slot load[LATENCY_CORES];
#ifdef SUNSTONE_LATENCY_MODEL
static _Alignas(64) volatile uint64_t memory[LATENCY_CORES][LATENCY_BYTES_PER_CORE / 8];
#else
static volatile uint64_t (*memory)[LATENCY_BYTES_PER_CORE / 8];
void latency_workload_bind(void *buffer)
{ memory = buffer; }
#endif
static StaticTask_t wake_state[LATENCY_CORES], load_state[LATENCY_CORES];
static StackType_t wake_stack[LATENCY_CORES][1024], load_stack[LATENCY_CORES][1024];
static TaskHandle_t wake_task[LATENCY_CORES], load_task[LATENCY_CORES];
static atomic_uint phase, ready_mask, stop_load;
static TickType_t began;
static struct latency_result *report;

__attribute__((weak)) void latency_workload_progress(void) {}
#if defined(SUNSTONE_LATENCY_DMA)
static void latency_dma_model_tick(void) {}
#define latency_dma_model_start latency_dma_native_start
#define latency_dma_model_stop latency_dma_native_stop
#elif !defined(SUNSTONE_LATENCY_MODEL)
static void latency_dma_model_tick(void) {}
static void latency_dma_model_start(void) {}
static unsigned latency_dma_model_stop(void) { return 0; }
#endif

/* The kernel calls this after ready-list insertion while its locks exclude
 * changes to the running task. Admit a sample only when its target core runs
 * the idle task or this diagnostic's memory task, as the phase requires. */
void ss_latency_task_ready(void *task)
{
    const uint64_t ready = ss_latency_now();
    unsigned p = atomic_load_explicit(&phase, memory_order_acquire);
    if (p != IDLE && p != BUSY) return;
    for (unsigned c = 0; c < LATENCY_CORES; ++c) {
        if (task != wake_task[c] || !atomic_load_explicit(&wake[c].pending, memory_order_acquire)) continue;
        TaskHandle_t running = xTaskGetCurrentTaskHandleForCore(c);
        if (p == IDLE) {
            /* Some model configurations let idle tasks migrate. Identity as
             * any kernel idle task establishes that this core is idle. */
            unsigned idle;
            for (idle = 0; idle < LATENCY_CORES; ++idle)
                if (running == xTaskGetIdleTaskHandleForCore(idle)) break;
            if (idle == LATENCY_CORES) return;
        } else if (running != load_task[c]) return;
        wake[c].released = ready;
        wake[c].metric = p == IDLE ? SS_LAT_SCHED_IDLE : SS_LAT_SCHED_BUSY;
        return;
    }
}
/* CPU0's periodic interrupt releases each pinned task. Remote-core samples
 * include SGI delivery. One outstanding sample prevents overwritten stamps. */
void ss_latency_tick_hook(void)
{
    unsigned p = atomic_load_explicit(&phase, memory_order_acquire);
    if (p == BUSY) latency_dma_model_tick();
    if (p != IDLE && p != BUSY) return;
    unsigned limit = p == IDLE ? LATENCY_IDLE_SAMPLES : LATENCY_IDLE_SAMPLES + LATENCY_BUSY_SAMPLES;
    BaseType_t woken = pdFALSE;
    for (unsigned c = 0; c < LATENCY_CORES; ++c) {
        if (atomic_load_explicit(&wake[c].completed, memory_order_acquire) >= limit ||
            atomic_load_explicit(&wake[c].pending, memory_order_acquire)) continue;
        wake[c].metric = SS_LAT_METRICS;
        atomic_store_explicit(&wake[c].pending, 1, memory_order_release);
        vTaskNotifyGiveFromISR(wake_task[c], &woken);
    }
    portYIELD_FROM_ISR(woken);
    /* FromISR mask restoration can clear PSTATE.I. The following production
     * tick handler requires the masked state supplied by IRQ entry. */
    __asm__ volatile("msr daifset, #2" ::: "memory");
}
static void wake_worker(void *argument)
{
    unsigned c = (unsigned)(uintptr_t)argument;
    atomic_fetch_or_explicit(&ready_mask, 1U << c, memory_order_release);
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        configASSERT(portGET_CORE_ID() == c);
        if (atomic_load_explicit(&phase, memory_order_acquire) == STOP) {
            ss_latency_freeze(c);
            atomic_store_explicit(&wake[c].parked, 1, memory_order_release);
            vTaskSuspend(NULL);
            configASSERT(0);
        }
        configASSERT(atomic_load_explicit(&wake[c].pending, memory_order_acquire));
        if (wake[c].metric < SS_LAT_METRICS) {
            const uint64_t running = ss_latency_now();
            unsigned index = atomic_load_explicit(&wake[c].completed, memory_order_relaxed);
            configASSERT(index < LATENCY_IDLE_SAMPLES + LATENCY_BUSY_SAMPLES);
            report->wakes[c][index] = (struct latency_wake_sample){
                wake[c].released, running, wake[c].metric,
                wake[c].metric == SS_LAT_SCHED_IDLE ? LATENCY_WAKE_IDLE : LATENCY_WAKE_BUSY};
            ss_latency_record_at(c, wake[c].metric, wake[c].released, running, (uintptr_t)wake_worker);
            atomic_fetch_add_explicit(&wake[c].completed, 1, memory_order_release);
        }
        atomic_store_explicit(&wake[c].pending, 0, memory_order_release);
    }
}
static void load_worker(void *argument)
{
    unsigned c = (unsigned)(uintptr_t)argument;
#ifndef SUNSTONE_LATENCY_MODEL
    for (unsigned i = 0; i < LATENCY_BYTES_PER_CORE / 8; ++i) memory[c][i] = 0;
#endif
    atomic_store_explicit(&load[c].ready, 1, memory_order_release);
    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    /* Every sweep reads and rewrites every word in a private 1 MiB extent.
     * Volatile accesses preserve that traffic even in optimized builds. */
    uint64_t digest = 0, sweep = 0;
    while (!atomic_load_explicit(&stop_load, memory_order_acquire)) {
        configASSERT(portGET_CORE_ID() == c);
        for (unsigned i = 0; i < LATENCY_BYTES_PER_CORE / 8; ++i) {
            uint64_t old = memory[c][i];
            configASSERT(old == sweep);
            digest += old ^ ((uint64_t)c << 32) ^ i;
            memory[c][i] = sweep + 1;
        }
        ++sweep;
        atomic_store_explicit(&load[c].sweeps, sweep, memory_order_release);
    }
    load[c].digest = digest;
    atomic_store_explicit(&load[c].stopped, 1, memory_order_release);
    vTaskSuspend(NULL);
    configASSERT(0);
}
static void wait_tick(void)
{
#ifdef SUNSTONE_LATENCY_MODEL
    if ((TickType_t)(xTaskGetTickCount() - began) >= pdMS_TO_TICKS(10000)) {
        platform_puts("FAIL latency phase="); platform_hex(atomic_load(&phase));
        platform_puts(" ready="); platform_hex(atomic_load(&ready_mask));
        for (unsigned c = 0; c < LATENCY_CORES; ++c) {
            platform_puts("\ncore="); platform_hex(c);
            platform_puts(" samples="); platform_hex(atomic_load(&wake[c].completed));
            platform_puts(" pending="); platform_hex(atomic_load(&wake[c].pending));
            platform_puts(" load_ready="); platform_hex(atomic_load(&load[c].ready));
            platform_puts(" sweeps="); platform_hex(atomic_load(&load[c].sweeps));
        }
        platform_puts("\n");
    }
#endif
    configASSERT((TickType_t)(xTaskGetTickCount() - began) < pdMS_TO_TICKS(10000));
    latency_workload_progress();
    vTaskDelay(1);
}
static void wait_samples(unsigned expected)
{
    for (unsigned c = 0; c < LATENCY_CORES; ++c)
        while (atomic_load_explicit(&wake[c].completed, memory_order_acquire) < expected ||
               atomic_load_explicit(&wake[c].pending, memory_order_acquire)) wait_tick();
}
void latency_workload_init(void)
{
    for (unsigned c = 0; c < LATENCY_CORES; ++c) {
        wake_task[c] = xTaskCreateStaticAffinitySet(wake_worker, "latency-wake", 1024,
            (void *)(uintptr_t)c, configMAX_PRIORITIES - 1, wake_stack[c], &wake_state[c], 1U << c);
        load_task[c] = xTaskCreateStaticAffinitySet(load_worker, "latency-memory", 1024,
            (void *)(uintptr_t)c, 1, load_stack[c], &load_state[c], 1U << c);
        configASSERT(wake_task[c] && load_task[c]);
    }
}
void latency_workload_run(struct latency_result *out)
{
    report = out;
    began = xTaskGetTickCount();
    while (atomic_load_explicit(&ready_mask, memory_order_acquire) != 0xff) wait_tick();
    for (unsigned c = 0; c < LATENCY_CORES; ++c)
        while (!atomic_load_explicit(&load[c].ready, memory_order_acquire)) wait_tick();
    ss_latency_phase_set(SS_LAT_IDLE);
    atomic_store_explicit(&phase, IDLE, memory_order_release);
    wait_samples(LATENCY_IDLE_SAMPLES);
    atomic_store_explicit(&phase, QUIET, memory_order_release);
    ss_latency_phase_set(SS_LAT_PHASE_NONE);
    for (unsigned c = 0; c < LATENCY_CORES; ++c) xTaskNotifyGive(load_task[c]);
    /* Require a full sweep on every core before taking loaded samples. */
    for (unsigned c = 0; c < LATENCY_CORES; ++c)
        while (!atomic_load_explicit(&load[c].sweeps, memory_order_acquire)) wait_tick();
    latency_dma_model_start();
    ss_latency_phase_set(SS_LAT_BUSY);
    atomic_store_explicit(&phase, BUSY, memory_order_release);
    wait_samples(LATENCY_IDLE_SAMPLES + LATENCY_BUSY_SAMPLES);
    ss_latency_phase_set(SS_LAT_PHASE_NONE);
    /* Measurement is complete. Stop background traffic before draining the
     * modeled DMA client; completion IRQs remain enabled throughout cleanup. */
    atomic_store_explicit(&stop_load, 1, memory_order_release);
    for (unsigned c = 0; c < LATENCY_CORES; ++c)
        while (!atomic_load_explicit(&load[c].stopped, memory_order_acquire)) wait_tick();
    out->dma_jobs = latency_dma_model_stop();
    atomic_store_explicit(&phase, QUIET, memory_order_release);
    atomic_store_explicit(&phase, STOP, memory_order_release);
    for (unsigned c = 0; c < LATENCY_CORES; ++c) xTaskNotifyGive(wake_task[c]);
    for (unsigned c = 0; c < LATENCY_CORES; ++c)
        while (!atomic_load_explicit(&wake[c].parked, memory_order_acquire)) wait_tick();
    /* Every core has disabled its recorder, including future idle and timer
     * hooks. No mutable metric is read while a writer can still change it. */
    ss_latency_snapshot(out->core);
    ss_latency_phase_snapshot(out->phases);
    for (unsigned c = 0; c < LATENCY_CORES; ++c)
        ss_latency_lock_counts(c, out->acquisitions[c]);
    for (unsigned p = 0; p < SS_LAT_PHASES; ++p)
        for (unsigned c = 0; c < LATENCY_CORES; ++c)
            ss_latency_phase_lock_counts(p, c, out->phase_acquisitions[p][c]);
    ss_latency_phase_bounds(out->phase_bounds);
    for (unsigned c = 0; c < LATENCY_CORES; ++c) out->wake_site[c] = (uintptr_t)wake_worker;
    for (unsigned c = 0; c < LATENCY_CORES; ++c) {
        uint64_t sweeps = atomic_load_explicit(&load[c].sweeps, memory_order_acquire);
        configASSERT(sweeps >= 2);
        out->traffic[c] = (struct latency_traffic){sweeps, sweeps * LATENCY_BYTES_PER_CORE,
            sweeps * LATENCY_BYTES_PER_CORE, load[c].digest};
    }
    out->magic = LATENCY_RESULT_MAGIC;
    out->frequency = ss_latency_counter_frequency();
    out->elapsed_ticks = (TickType_t)(xTaskGetTickCount() - began);
    out->core_mask = 0xff;
    out->timer_irq_core_mask = 1;
#ifdef SUNSTONE_LATENCY_MODEL
    out->model = 1;
#endif
}
