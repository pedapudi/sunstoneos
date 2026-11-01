#ifndef SUNSTONE_LATENCY_H
#define SUNSTONE_LATENCY_H
#include <stdint.h>

enum ss_latency_region {
    SS_LAT_IRQ, SS_LAT_MASKED, SS_LAT_LOCK_WAIT0, SS_LAT_LOCK_HOLD0,
    SS_LAT_LOCK_WAIT1, SS_LAT_LOCK_HOLD1, SS_LAT_SCHED_IDLE, SS_LAT_SCHED_BUSY,
    SS_LAT_DMA, SS_LAT_GPU, SS_LAT_NPU, SS_LAT_METRICS
};
struct ss_latency_metric {
    uint64_t count, maximum, start, site;
    uint64_t first_over_start, first_over_ticks, first_over_site;
};
struct ss_latency_core {
    struct ss_latency_metric metric[SS_LAT_METRICS];
} __attribute__((aligned(64)));

#ifdef SUNSTONE_LATENCY
static inline uint64_t ss_latency_now(void)
{
    uint64_t value;
    __asm__ volatile("isb\n mrs %0, cntvct_el0" : "=r"(value) :: "memory");
    return value;
}
extern struct ss_latency_core ss_latency_results[8];
extern uint64_t ss_latency_frequency;
extern const uint64_t ss_latency_budget_us[SS_LAT_METRICS];
/* Initialize once before measurement; each core freezes itself before the
 * coordinator snapshots. A region has one writer per core, including IRQs. */
void ss_latency_init(uint64_t frequency);
uint64_t ss_latency_counter_frequency(void);
void ss_latency_freeze(unsigned core);
void ss_latency_snapshot(struct ss_latency_core out[8]);
void ss_latency_record(unsigned core, unsigned region, uint64_t start, uintptr_t site);
void ss_latency_record_at(unsigned core, unsigned region, uint64_t start,
                          uint64_t end, uintptr_t site);
void ss_latency_mask_open(unsigned core, uint64_t start, uintptr_t site);
void ss_latency_mask_close(unsigned core);
void ss_latency_lock_acquired(unsigned core, unsigned lock, uint64_t start,
                               uintptr_t site);
void ss_latency_lock_releasing(unsigned core, unsigned lock);
/* One outstanding masked interrupt source per metric. Coalesced IRQs retain
 * the earliest timestamp until the worker completes its observation pass. */
void ss_latency_event_begin(unsigned region);
void ss_latency_event_end(unsigned region);
uint64_t ss_latency_event_take(unsigned region);
void ss_latency_event_finish(unsigned region, uint64_t start);
void ss_latency_gpu_arm(void);
void ss_latency_gpu_post(unsigned client);
void ss_latency_gpu_received(unsigned client);
void ss_latency_tick_hook(void);
void ss_latency_task_ready(void *task);
#define SS_LATENCY_RECORD(core, region, start, site) \
    ss_latency_record((core), (region), (start), (uintptr_t)(site))
#define SS_LATENCY_EVENT_BEGIN(region) ss_latency_event_begin(region)
#define SS_LATENCY_EVENT_END(region) ss_latency_event_end(region)
#define SS_LATENCY_EVENT_CLAIM(name, region) uint64_t name = ss_latency_event_take(region)
#define SS_LATENCY_EVENT_FINISH(name, region) ss_latency_event_finish((region), (name))
#define SS_LATENCY_GPU_ARM() ss_latency_gpu_arm()
#define SS_LATENCY_GPU_POST(client) ss_latency_gpu_post(client)
#define SS_LATENCY_GPU_RECEIVED(client) ss_latency_gpu_received(client)
#else
#define SS_LATENCY_RECORD(core, region, start, site) ((void)0)
#define SS_LATENCY_EVENT_BEGIN(region) ((void)0)
#define SS_LATENCY_EVENT_END(region) ((void)0)
#define SS_LATENCY_EVENT_CLAIM(name, region)
#define SS_LATENCY_EVENT_FINISH(name, region) ((void)0)
#define SS_LATENCY_GPU_ARM() ((void)0)
#define SS_LATENCY_GPU_POST(client) ((void)0)
#define SS_LATENCY_GPU_RECEIVED(client) ((void)0)
#endif
#endif
