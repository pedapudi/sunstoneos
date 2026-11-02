#ifndef SUNSTONE_LATENCY_H
#define SUNSTONE_LATENCY_H
#include <stdint.h>

enum ss_latency_region {
    SS_LAT_IRQ, SS_LAT_MASKED, SS_LAT_LOCK_WAIT0, SS_LAT_LOCK_HOLD0,
    SS_LAT_LOCK_WAIT1, SS_LAT_LOCK_HOLD1, SS_LAT_SCHED_IDLE, SS_LAT_SCHED_BUSY,
    SS_LAT_DMA, SS_LAT_GPU, SS_LAT_NPU, SS_LAT_METRICS
};
enum ss_latency_phase {
    SS_LAT_INIT, SS_LAT_IDLE, SS_LAT_BUSY, SS_LAT_PHASES, SS_LAT_PHASE_NONE
};
/* Nearest-rank quantiles are upper endpoints of logarithmic bins, with sixteen
 * subdivisions per power of two (at most 6.25% width). Zero is exact; the
 * final bin ends at UINT64_MAX. Maximum and its provenance are exact. */
struct ss_latency_distribution {
    uint64_t count, p50_upper, p99_upper, maximum, start, site;
    uint64_t overflow_bin_count; /* Samples whose bin upper endpoint is UINT64_MAX. */
};
struct ss_latency_phase_core {
    struct ss_latency_distribution metric[SS_LAT_METRICS];
};
struct ss_latency_metric {
    uint64_t count, maximum, start, site;
    uint64_t first_over_start, first_over_ticks, first_over_site;
};
struct ss_latency_core {
    struct ss_latency_metric metric[SS_LAT_METRICS];
} __attribute__((aligned(64)));
#define SS_LAT_SITE_KINDS 2U
#define SS_LAT_SITE_SLOTS 3U
/* Longest nonzero masked sections (kind 0) and task-lock holds (kind 1).
 * Sites are return addresses at entry and exit. Tables cover each phase and
 * an aggregate at index SS_LAT_PHASES, including gaps and crossing intervals. */
struct ss_latency_site {
    uint64_t ticks, start, enter_site, exit_site;
};

/* Optional PMU capture (SUNSTONE_LATENCY_PMU, off by default): the counts a core
 * executed inside each of its longest masked spans, parallel to ss_latency_site
 * entries of kind 0. Differences are modulo 2^32. */
struct ss_latency_site_pmu {
    uint32_t instructions, cycles, l1d_refills, l2d_refills;
};

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
/* Snapshot only after every writer has frozen; packed in descending duration. */
void ss_latency_sites_snapshot(struct ss_latency_site out[SS_LAT_PHASES + 1][8][SS_LAT_SITE_KINDS][SS_LAT_SITE_SLOTS]);
#ifdef SUNSTONE_LATENCY_PMU
/* Same order as the kind-0 site table; zero for a span measured before the
 * core's counters were enabled. Enable runs from the IRQ hook at EL1. */
void ss_latency_site_pmu_snapshot(struct ss_latency_site_pmu out[SS_LAT_PHASES + 1][8][SS_LAT_SITE_SLOTS]);
void ss_latency_pmu_enable(void);
#endif
/* Single coordinator; each named phase opens once. Only intervals wholly inside
 * a phase enter its distribution. Crossing intervals and gaps retain their
 * aggregate record. Bounds use a common counter across all cores. */
void ss_latency_phase_set(unsigned phase);
void ss_latency_phase_snapshot(struct ss_latency_phase_core out[SS_LAT_PHASES][8]);
/* Call on this core with recording excluded, or after it has frozen. Counts
 * include recursive acquisitions; wait/hold samples cover outer acquisitions. */
void ss_latency_lock_counts(unsigned core, uint64_t out[2]);
/* Phase counts use the existing acquisition-request timestamp, including recursion. */
void ss_latency_phase_lock_counts(unsigned phase, unsigned core, uint64_t out[2]);
void ss_latency_phase_bounds(uint64_t out[SS_LAT_PHASES][2]);
void ss_latency_record(unsigned core, unsigned region, uint64_t start, uintptr_t site);
void ss_latency_record_at(unsigned core, unsigned region, uint64_t start,
                          uint64_t end, uintptr_t site);
void ss_latency_mask_open(unsigned core, uint64_t start, uintptr_t site);
void ss_latency_mask_close(unsigned core, uintptr_t site);
void ss_latency_lock_acquired(unsigned core, unsigned lock, uint64_t start,
                               uintptr_t site);
void ss_latency_lock_releasing(unsigned core, unsigned lock, uintptr_t site);
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
