#include "FreeRTOS.h"
#include "task.h"
#include "platform.h"
#include "workload.h"

#ifdef SUNSTONE_LATENCY_CPU_ONLY
/* The CPU control has no DMA client; these hooks stand for the board image's absence of one. */
void latency_dma_model_tick(void) {}
void latency_dma_model_start(void) {}
unsigned latency_dma_model_stop(void) { return 0; }
#endif
static struct latency_result result;
static StaticTask_t controller_state;
static StackType_t controller_stack[2048];
static const char *const metric_names[SS_LAT_METRICS] = {
    "timer_irq", "irq_masked", "lock0_wait", "lock0_hold", "lock1_wait", "lock1_hold",
    "wake_idle", "wake_busy", "DMA_completion", "GPU_completion", "NPU_completion"
};
static void number(const char *name, uint64_t value)
{ platform_puts(name); platform_hex(value); }
static void controller(void *argument)
{
    (void)argument;
    latency_workload_run(&result);
#ifdef SUNSTONE_SCHED_LATENCY_TRACE
    configASSERT(result.scheduler.magic == SS_SCHED_TRACE_MAGIC);
    configASSERT(result.scheduler.version == 1);
    configASSERT(result.scheduler.tick_count && result.scheduler.tick_count <= SS_SCHED_TRACE_TICKS);
    configASSERT(!result.scheduler.tick_dropped || result.scheduler.tick_count == SS_SCHED_TRACE_TICKS);
    for (unsigned c = 0; c < LATENCY_CORES; ++c) {
        const struct ss_sched_trace_core *trace = &result.scheduler.core[c];
        configASSERT(trace->command_count && trace->drain_count && trace->irq_count);
        configASSERT(trace->self_suspend.command == 2);
        configASSERT((trace->self_suspend.flags & 5) == 5);
        configASSERT(trace->self_suspend.stamp[0] && trace->self_suspend.stamp[6]);
        for (unsigned i = 1; i < 7; ++i)
            configASSERT(trace->self_suspend.stamp[i] >= trace->self_suspend.stamp[i - 1]);
        configASSERT(!trace->drain.dropped);
        for (unsigned i = 0; i < trace->drain.count; ++i) {
            configASSERT(trace->drain.record[i].stamp[0] >= trace->drain.start);
            configASSERT(trace->drain.record[i].stamp[4] <= trace->drain.end);
        }
    }
    platform_puts("PASS scheduler trace: eight self-suspends, drain records, IRQ exits and timer intervals\n");
#endif
    platform_puts("MODEL latency: host QEMU timing; CPU0 timer IRQ releases eight pinned tasks; DMA hardware modeled\n");
    platform_puts("DMA model stimulus: continuous 4096-byte jobs; client priority1 and service priority4 on CPU0\n");
    number("frequency=", result.frequency);
    number(" elapsed_ticks=", result.elapsed_ticks);
    number(" modeled_DMA_jobs=", result.dma_jobs);
    platform_puts("\n");
    for (unsigned c = 0; c < LATENCY_CORES; ++c) {
        configASSERT(result.core[c].metric[SS_LAT_SCHED_IDLE].count == LATENCY_IDLE_SAMPLES);
        configASSERT(result.core[c].metric[SS_LAT_SCHED_BUSY].count == LATENCY_BUSY_SAMPLES);
        number("core=", c);
        number(" sweeps=", result.traffic[c].sweeps);
        number(" bytes_read=", result.traffic[c].bytes_read);
        number(" bytes_written=", result.traffic[c].bytes_written);
        number(" digest=", result.traffic[c].digest);
        platform_puts("\n");
        number("isr_acquisitions=", result.acquisitions[c][0]);
        number(" task_acquisitions=", result.acquisitions[c][1]);
        platform_puts("\n");
        for (unsigned i = 0; i < LATENCY_IDLE_SAMPLES + LATENCY_BUSY_SAMPLES; ++i) {
            const struct latency_wake_sample *w = &result.wakes[c][i];
            unsigned p = i < LATENCY_IDLE_SAMPLES ? SS_LAT_IDLE : SS_LAT_BUSY;
            configASSERT(w->metric == (p == SS_LAT_IDLE ? SS_LAT_SCHED_IDLE : SS_LAT_SCHED_BUSY));
            configASSERT(w->workload_flags == (p == SS_LAT_IDLE ? LATENCY_WAKE_IDLE : LATENCY_WAKE_BUSY));
            configASSERT(result.phase_bounds[p][0] <= w->ready && w->ready <= w->running);
            configASSERT(w->running <= result.phase_bounds[p][1]);
        }
        for (unsigned p = 0; p < SS_LAT_PHASES; ++p)
            for (unsigned m = 0; m < SS_LAT_METRICS; ++m) {
                const struct ss_latency_distribution *d = &result.phases[p][c].metric[m];
                number("phase=", p); number(" metric=", m);
                number(" count=", d->count);
                number(" p50_upper_ticks=", d->p50_upper);
                number(" p99_upper_ticks=", d->p99_upper);
                number(" max_ticks=", d->maximum);
                number(" max_start=", d->start); number(" max_site=", d->site);
                number(" overflow_bin_count=", d->overflow_bin_count);
                platform_puts("\n");
            }
        for (unsigned p = 0; p <= SS_LAT_PHASES; ++p)
            for (unsigned k = 0; k < SS_LAT_SITE_KINDS; ++k) {
                uint64_t previous = UINT64_MAX;
                for (unsigned i = 0; i < SS_LAT_SITE_SLOTS; ++i) {
                    const struct ss_latency_site *s = &result.sites[p][c][k][i];
                    configASSERT(s->ticks <= previous);
                    previous = s->ticks;
                    if (!s->ticks) {
                        configASSERT(!s->start && !s->enter_site && !s->exit_site);
                        continue;
                    }
                    configASSERT(s->enter_site && s->exit_site);
                    unsigned metric = k == 0 ? SS_LAT_MASKED : SS_LAT_LOCK_HOLD1;
                    configASSERT(s->ticks <= result.core[c].metric[metric].maximum);
                    if (p < SS_LAT_PHASES) {
                        configASSERT(result.phase_bounds[p][0] <= s->start);
                        configASSERT(s->start < result.phase_bounds[p][1]);
                        configASSERT(s->ticks <= result.phase_bounds[p][1] - s->start);
                    }
#ifdef SUNSTONE_LATENCY_PMU
                    if (k == 0 && (p == SS_LAT_BUSY || p == SS_LAT_PHASES)) {
                        const struct ss_latency_site_pmu *q = &result.site_pmu[p == SS_LAT_BUSY ? 0 : 1][c][i];
                        number("site_pmu instructions=", q->instructions); number(" cycles=", q->cycles);
                        number(" stall_backend=", q->stall_backend); number(" mem_access=", q->mem_access);
                        number(" bus_access=", q->bus_access); number(" l1d_refills=", q->l1d_refills);
                        platform_puts("\n");
                    }
#endif
                    number("site_phase=", p); number(" kind=", k); number(" slot=", i);
                    number(" ticks=", s->ticks); number(" start=", s->start);
                    number(" enter_site=", s->enter_site); number(" exit_site=", s->exit_site);
                    platform_puts("\n");
                }
            }
        for (unsigned m = 0; m < SS_LAT_METRICS; ++m) {
            const struct ss_latency_metric *sample = &result.core[c].metric[m];
            platform_puts(metric_names[m]);
            number(" count=", sample->count);
            number(" max_ticks=", sample->maximum);
            number(" max_start=", sample->start);
            number(" max_site=", sample->site);
            number(" first_over_start=", sample->first_over_start);
            number(" first_over_ticks=", sample->first_over_ticks);
            number(" first_over_site=", sample->first_over_site);
            platform_puts("\n");
        }
    }
    configASSERT(result.core[0].metric[SS_LAT_IRQ].count);
#ifndef SUNSTONE_LATENCY_CPU_ONLY
    configASSERT(result.core[0].metric[SS_LAT_DMA].count);
#endif
    platform_puts("PASS latency model: 64 idle and 128 loaded wake samples on each of eight cores; sequential 1 MiB/core traffic and real DMA service with modeled completion\n");
    platform_exit(0);
}
void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{ (void)task; platform_panic(name, 0); }
void kernel_main(void)
{
    platform_init();
    uint64_t frequency;
    __asm__ volatile("mrs %0,cntfrq_el0" : "=r"(frequency));
    ss_latency_init(frequency);
    latency_workload_init();
#ifndef SUNSTONE_LATENCY_CPU_ONLY
    latency_dma_model_init();
#endif
    configASSERT(xTaskCreateStaticAffinitySet(controller, "latency-control", 2048, NULL, 5,
        controller_stack, &controller_state, 1));
    vTaskStartScheduler();
    platform_panic("latency scheduler returned", 0);
}
