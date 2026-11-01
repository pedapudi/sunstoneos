#include "FreeRTOS.h"
#include "task.h"
#include "platform.h"
#include "workload.h"

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
    platform_puts("MODEL latency: host QEMU timing; CPU0 timer IRQ releases eight pinned tasks; DMA hardware modeled\n");
    platform_puts("DMA model stimulus: continuous 4096-byte jobs; client priority1 and service priority4 on CPU0\n");
    number("frequency=", result.frequency);
    number(" elapsed_ticks=", result.elapsed_ticks);
    number(" modeled_DMA_jobs=", result.modeled_dma_jobs);
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
    configASSERT(result.core[0].metric[SS_LAT_DMA].count);
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
    latency_dma_model_init();
    configASSERT(xTaskCreateStaticAffinitySet(controller, "latency-control", 2048, NULL, 5,
        controller_stack, &controller_state, 1));
    vTaskStartScheduler();
    platform_panic("latency scheduler returned", 0);
}
