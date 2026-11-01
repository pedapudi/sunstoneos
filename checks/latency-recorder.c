/* Deterministic interval inputs test maximum and first-failure provenance. */
#include "FreeRTOS.h"
#include "task.h"
#include "platform.h"
#include "sunstone_latency.h"
extern _Noreturn void platform_exit_privileged(unsigned status);
static struct ss_latency_core copy[8];
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
    for (unsigned c = 0; c < 8; ++c) ss_latency_freeze(c);
    ss_latency_record_at(0, SS_LAT_IRQ, 100, 10000, 999);
    ss_latency_snapshot(copy);
    configASSERT(copy[0].metric[SS_LAT_IRQ].count == 4);
    configASSERT(copy[1].metric[SS_LAT_IRQ].count == 0);
    platform_puts("PASS latency recorder maximum, nesting, first fault and freeze\n");
    platform_exit_privileged(0);
}
