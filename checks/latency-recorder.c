/* Deterministic interval inputs test maximum and first-failure provenance. */
#include "FreeRTOS.h"
#include "task.h"
#include "platform.h"
#include "sunstone_latency.h"
extern _Noreturn void platform_exit_privileged(unsigned status);
static struct ss_latency_core copy[8];
static struct ss_latency_phase_core phases[SS_LAT_PHASES][8];
void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{ (void)task; platform_panic(name, 1); }
void kernel_main(void)
{
    ss_latency_event_begin(SS_LAT_DMA); /* Startup must not seed a sample. */
    ss_latency_mask_open(0, 0, 1);
    ss_latency_init(1000000);
    configASSERT(ss_latency_event_take(SS_LAT_DMA) == 0);
    ss_latency_mask_close(0);
    configASSERT(ss_latency_results[0].metric[SS_LAT_MASKED].count == 0);
    ss_latency_record_at(0, SS_LAT_IRQ, 100, 110, 1);
    ss_latency_record_at(0, SS_LAT_IRQ, 200, 270, 77);
    ss_latency_record_at(0, SS_LAT_IRQ, 300, 390, 88);
    ss_latency_record_at(0, SS_LAT_IRQ, 400, 450, 99);
    ss_latency_record_at(0, SS_LAT_IRQ, 500, 499, 100);
    ss_latency_record_at(8, SS_LAT_IRQ, 1, 2, 3);
    ss_latency_record_at(0, SS_LAT_METRICS, 1, 2, 3);
    struct ss_latency_metric *m = &ss_latency_results[0].metric[SS_LAT_IRQ];
    configASSERT(m->count == 4 && m->maximum == 90 && m->site == 88 && m->start == 300);
    configASSERT(m->first_over_start == 200 && m->first_over_ticks == 70 && m->first_over_site == 77);
    ss_latency_mask_open(0, ss_latency_now(), 123);
    ss_latency_mask_open(0, ss_latency_now(), 456);
    ss_latency_mask_close(0);
    ss_latency_mask_close(0);
    m = &ss_latency_results[0].metric[SS_LAT_MASKED];
    configASSERT(m->count == 1 && m->site == 123);
    ss_latency_lock_acquired(0, 0, ss_latency_now(), 789);
    ss_latency_lock_acquired(0, 0, ss_latency_now(), 999);
    ss_latency_lock_releasing(0, 0);
    configASSERT(ss_latency_results[0].metric[SS_LAT_LOCK_HOLD0].count == 0);
    ss_latency_lock_releasing(0, 0);
    configASSERT(ss_latency_results[0].metric[SS_LAT_LOCK_HOLD0].count == 1);
    configASSERT(ss_latency_results[0].metric[SS_LAT_LOCK_WAIT0].count == 1);
    configASSERT(ss_latency_results[0].metric[SS_LAT_LOCK_HOLD0].site == 789);
    ss_latency_event_begin(SS_LAT_DMA);
    uint64_t first = ss_latency_event_take(SS_LAT_DMA);
    ss_latency_event_begin(SS_LAT_DMA); /* IRQ during the worker's sweep. */
    ss_latency_event_finish(SS_LAT_DMA, first);
    configASSERT(first && ss_latency_event_take(SS_LAT_DMA));
    uint64_t counts[2];
    ss_latency_lock_counts(0, counts);
    configASSERT(counts[0] == 2 && counts[1] == 0);
    ss_latency_phase_set(SS_LAT_IDLE);
    uint64_t windows[SS_LAT_PHASES][2];
    ss_latency_phase_bounds(windows);
    uint64_t original_idle = windows[SS_LAT_IDLE][0];
    ss_latency_phase_set(SS_LAT_IDLE); /* Reopening cannot rewrite a window. */
    ss_latency_phase_bounds(windows);
    configASSERT(windows[SS_LAT_IDLE][0] == original_idle);
    uint64_t idle_start = ss_latency_now();
    for (unsigned i = 0; i <= 100; ++i)
        ss_latency_record_at(7, SS_LAT_NPU, idle_start, idle_start + i, 1000 + i);
    ss_latency_phase_set(SS_LAT_PHASE_NONE);
    uint64_t gap_start = ss_latency_now();
    ss_latency_record_at(7, SS_LAT_NPU, gap_start, gap_start + 9999, 9999);
    ss_latency_phase_set(SS_LAT_BUSY);
    /* A crossing interval remains in the aggregate only. */
    ss_latency_record_at(7, SS_LAT_DMA, idle_start, ss_latency_now(), 222);
    uint64_t busy_start = ss_latency_now();
    ss_latency_record_at(7, SS_LAT_NPU, busy_start, busy_start + UINT64_MAX - busy_start, 333);
    /* A majority of zeros must keep an exact zero median. */
    for (unsigned i = 0; i < 3; ++i)
        ss_latency_record_at(6, SS_LAT_NPU, busy_start, busy_start, 444);
    ss_latency_record_at(5, SS_LAT_GPU, busy_start, busy_start, 666);
    ss_latency_record_at(6, SS_LAT_NPU, busy_start, busy_start + 3, 555);
    for (unsigned c = 0; c < 8; ++c) ss_latency_freeze(c);
    ss_latency_record_at(0, SS_LAT_IRQ, 100, 10000, 999);
    ss_latency_snapshot(copy);
    ss_latency_phase_snapshot(phases);
    const struct ss_latency_distribution *d = &phases[SS_LAT_IDLE][7].metric[SS_LAT_NPU];
    configASSERT(d->count == 101 && d->p50_upper == 51 && d->p99_upper == 99);
    configASSERT(d->maximum == 100 && d->start == idle_start && d->site == 1100);
    configASSERT(copy[7].metric[SS_LAT_NPU].count == 103);
    configASSERT(phases[SS_LAT_INIT][0].metric[SS_LAT_IRQ].count == 4);
    configASSERT(phases[SS_LAT_IDLE][7].metric[SS_LAT_DMA].count == 0);
    configASSERT(phases[SS_LAT_BUSY][7].metric[SS_LAT_DMA].count == 0);
    configASSERT(phases[SS_LAT_BUSY][7].metric[SS_LAT_NPU].count == 1);
    configASSERT(phases[SS_LAT_BUSY][7].metric[SS_LAT_NPU].p99_upper == UINT64_MAX);
    configASSERT(phases[SS_LAT_BUSY][7].metric[SS_LAT_NPU].overflow_bin_count == 1);
    d = &phases[SS_LAT_BUSY][6].metric[SS_LAT_NPU];
    configASSERT(d->p50_upper == 0 && d->p99_upper == 3 && d->maximum == 3);
    configASSERT(phases[SS_LAT_BUSY][5].metric[SS_LAT_GPU].start == busy_start);
    configASSERT(phases[SS_LAT_BUSY][5].metric[SS_LAT_GPU].site == 666);
    configASSERT(copy[0].metric[SS_LAT_IRQ].count == 4);
    configASSERT(copy[1].metric[SS_LAT_IRQ].count == 0);
    platform_puts("PASS latency recorder maximum, nesting, first fault, phase quantiles, origin and freeze\n");
    platform_exit_privileged(0);
}
