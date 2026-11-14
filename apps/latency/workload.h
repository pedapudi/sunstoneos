#ifndef SUNSTONE_LATENCY_WORKLOAD_H
#define SUNSTONE_LATENCY_WORKLOAD_H

#include "sunstone_latency.h"
#include <stddef.h>
#include <stdint.h>
#ifdef SUNSTONE_LATENCY_CONTROL
#include "control.h"
#endif
#ifdef SUNSTONE_SCHED_LATENCY_TRACE
#include "latency_trace.h"
#endif
#ifdef SUNSTONE_SCHED_PMU
#include "latency_pmu.h"
#endif
#ifdef SUNSTONE_LATENCY_CAUSE
#include "latency_cause.h"
#include "cause-ping.h"
#define LATENCY_CAUSE_FOOTER_MAGIC UINT64_C(0x5353434155534631)
#endif

#define LATENCY_CORES 8U
/* The manifest's output_limit (apps/latency/cmake/application.cmake): traced
 * and control profiles declare 131072 bytes and the cause profile 147456.
 * Each cause section adds its own allowance: service cycles 32768, band
 * probes 4096 and wake segments 32768. */
#ifdef SUNSTONE_LATENCY_CAUSE
#ifdef SUNSTONE_LATENCY_SERVICE_CYCLES
#define LATENCY_SERVICE_LIMIT 32768U
#else
#define LATENCY_SERVICE_LIMIT 0U
#endif
#ifdef SUNSTONE_LATENCY_BAND_SWITCH
#define LATENCY_BAND_LIMIT 4096U
#else
#define LATENCY_BAND_LIMIT 0U
#endif
#ifdef SUNSTONE_LATENCY_WAKE_SEGMENTS
#define LATENCY_WAKE_LIMIT 65536U
#else
#define LATENCY_WAKE_LIMIT 0U
#endif
#define LATENCY_RESULT_LIMIT (147456U + LATENCY_SERVICE_LIMIT + LATENCY_BAND_LIMIT + LATENCY_WAKE_LIMIT)
#else
#define LATENCY_RESULT_LIMIT 131072U
#endif
#define LATENCY_IDLE_SAMPLES 64U
#define LATENCY_BUSY_SAMPLES 128U
#define LATENCY_BYTES_PER_CORE (1024U * 1024U)
#define LATENCY_RESULT_MAGIC UINT64_C(0x53534c4154454e31)
#ifndef SUNSTONE_LATENCY_RUN_US
#define SUNSTONE_LATENCY_RUN_US 12000000u
#endif

struct latency_traffic {
    uint64_t sweeps, bytes_read, bytes_written, digest;
};
struct latency_wake_sample {
    uint64_t ready, running;
    uint32_t metric, workload_flags;
};
#define LATENCY_WAKE_IDLE 1U
#define LATENCY_WAKE_BUSY 2U
#ifdef SUNSTONE_LATENCY_BAND_SWITCH
/* The band probes (docs/designs/scheduler-partitioned.md, "Reschedule bands").
 * A probe task at priority 6 (band 3) on each of CPUs 1 and 2 spins on the
 * cycle counter in the busy phase and records every gap above
 * LATENCY_BAND_GAP_CYCLES. After each of its wakes, CPU3's wake task
 * notifies a priority-2 (band 1) background task on CPUs 1 and 2; the probe
 * keeps sampling for LATENCY_BAND_MARGIN_CYCLES after that notification, then
 * lowers itself to priority 1 so that the background task runs and blocks
 * again, and returns to priority 6. A gap in which the probe's own wake task
 * completed is the probe's own; any other is foreign. Each window is filed
 * under the band setting (off, on) that its core applied at the window's
 * start; a window whose core changed setting before its end is discarded. */
#define LATENCY_BAND_MAGIC UINT64_C(0x535342414e445331)
#define LATENCY_BAND_FOOTER_MAGIC UINT64_C(0x535342414e444631)
#define LATENCY_BAND_PROBES 2U
#define LATENCY_BAND_BINS 8U
#define LATENCY_BAND_GAP_CYCLES 300U
#define LATENCY_BAND_MARGIN_CYCLES 50000U
/* Foreign gaps by size: [300, 512), [512, 1024), ... [16384, 32768), then 32768 cycles and more. */
struct latency_band_cell {
    uint64_t windows, cycles;
    uint64_t foreign, foreign_irq, foreign_max, foreign_sum;
    uint64_t own, own_max;
    uint64_t bins[LATENCY_BAND_BINS];
};
_Static_assert(sizeof(struct latency_band_cell) == 128, "band cell layout");
struct latency_band_report {
    uint64_t magic, version, bytes, gap_cycles, margin_cycles, switch_shift, notified, wait_timeouts;
    uint64_t discarded[LATENCY_BAND_PROBES], background_runs[LATENCY_BAND_PROBES], unfinished[LATENCY_BAND_PROBES];
    uint64_t complete, reserved;
    /* Bit i of word i / 64: the band setting the core applied when wake sample i was recorded (1 on). */
    uint64_t wake_on[8][3];
    struct latency_band_cell cell[LATENCY_BAND_PROBES][2];
    /* CPU0's PMR write that changes the mask, 64 samples each in cycles,
     * taken with the cause benchmark: [0] the write alone, [1] the write,
     * DSB SY and ISB (the context restore's sequence). */
    uint64_t pmr_count[2], pmr_cycles[2][64];
};
_Static_assert(sizeof(struct latency_band_report) == 128 + 192 + 512 + 16 + 1024, "band report layout");
#endif
struct latency_result {
    uint64_t magic, frequency, elapsed_ticks;
    uint32_t core_mask, model, dma_jobs, timer_irq_core_mask;
    struct latency_traffic traffic[LATENCY_CORES];
    struct ss_latency_core core[LATENCY_CORES];
    struct ss_latency_phase_core phases[SS_LAT_PHASES][LATENCY_CORES];
    uint64_t acquisitions[LATENCY_CORES][2];
    uint64_t phase_acquisitions[SS_LAT_PHASES][LATENCY_CORES][2];
    uint64_t phase_bounds[SS_LAT_PHASES][2];
    uint64_t wake_site[LATENCY_CORES];
    struct latency_wake_sample wakes[LATENCY_CORES][LATENCY_IDLE_SAMPLES + LATENCY_BUSY_SAMPLES];
    struct ss_latency_site sites[SS_LAT_PHASES + 1][LATENCY_CORES][SS_LAT_SITE_KINDS][SS_LAT_SITE_SLOTS]
        __attribute__((aligned(64)));
#ifdef SUNSTONE_LATENCY_PMU
    /* Implemented-event mask of each core, then the counts inside the kind-0 sites above
     * (row 0 busy phase, row 1 aggregate; same core and slot). */
    uint32_t pmu_supported[LATENCY_CORES];
    struct ss_latency_site_pmu site_pmu[SS_LAT_PMU_ROWS][LATENCY_CORES][SS_LAT_SITE_SLOTS];
#endif
#ifdef SUNSTONE_SCHED_LATENCY_TRACE
    struct ss_sched_trace_report scheduler __attribute__((aligned(64)));
#endif
#ifdef SUNSTONE_LATENCY_CONTROL
    struct latency_control_report control __attribute__((aligned(64)));
#endif
#ifdef SUNSTONE_SCHED_PMU
    struct ss_sched_pmu_report pmu_windows __attribute__((aligned(64)));
#endif
#ifdef SUNSTONE_LATENCY_CAUSE
    struct ss_latency_cause_report cause __attribute__((aligned(64)));
    struct latency_cause_ping_report ping;
    uint64_t cause_footer[8] __attribute__((aligned(64)));
#endif
#ifdef SUNSTONE_LATENCY_SERVICE_CYCLES
    /* After the cause footer, so a decoder that strips this section's footer
     * and the section finds the cause report as before. */
    struct ss_service_cycles_report service __attribute__((aligned(64)));
    uint64_t service_footer[8] __attribute__((aligned(64)));
#endif
#ifdef SUNSTONE_LATENCY_BAND_SWITCH
    /* After the service footer, so a decoder that strips this section's
     * footer and the section finds the service section as before. */
    struct latency_band_report bands __attribute__((aligned(64)));
    uint64_t band_footer[8] __attribute__((aligned(64)));
#endif
#ifdef SUNSTONE_LATENCY_WAKE_SEGMENTS
    /* Last, with its own footer, for the same reason. */
    struct ss_wake_seg_report wake_segments __attribute__((aligned(64)));
    uint64_t wake_footer[8] __attribute__((aligned(64)));
#endif
};
_Static_assert(offsetof(struct latency_result, core) == 320 &&
    offsetof(struct latency_result, phases) == 5440 &&
    offsetof(struct latency_result, acquisitions) == 20224 &&
    offsetof(struct latency_result, wakes) == 20848 &&
    offsetof(struct latency_result, sites) == 57728 &&
#ifdef SUNSTONE_SCHED_LATENCY_TRACE
#ifdef SUNSTONE_LATENCY_PMU
    offsetof(struct latency_result, scheduler) == 65088 &&
#else
    offsetof(struct latency_result, scheduler) == 63872 &&
#endif
    sizeof(struct latency_result) <= LATENCY_RESULT_LIMIT,
#elif defined(SUNSTONE_LATENCY_CONTROL)
    offsetof(struct latency_result, control) == 63872 &&
    sizeof(struct latency_result) == 63872 + 17536,
#elif defined(SUNSTONE_LATENCY_PMU)
    sizeof(struct latency_result) == 63872 + 1216,
#else
    sizeof(struct latency_result) == 63872,
#endif
    "latency binary report layout");
#ifdef SUNSTONE_LATENCY_CONTROL
_Static_assert(offsetof(struct latency_result, control) % 64 == 0 &&
    sizeof(struct latency_result) == offsetof(struct latency_result, control) + 17536 &&
    sizeof(struct latency_result) <= LATENCY_RESULT_LIMIT, "control report tail layout");
#ifdef SUNSTONE_SCHED_LATENCY_TRACE
_Static_assert(offsetof(struct latency_result, control) ==
    ((offsetof(struct latency_result, scheduler) + sizeof(struct ss_sched_trace_report) + 63u) & ~(size_t)63u),
    "control tail follows the complete scheduler report");
#endif
#endif

#ifdef SUNSTONE_SCHED_PMU
_Static_assert(offsetof(struct latency_result, pmu_windows) % 64 == 0 &&
    sizeof(struct latency_result) == offsetof(struct latency_result, pmu_windows) + 11712 &&
    sizeof(struct latency_result) <= LATENCY_RESULT_LIMIT, "scheduler PMU report tail layout");
#endif

#ifdef SUNSTONE_LATENCY_CAUSE
/* Each cause section follows the footer before it, and the last footer ends the result. */
#define LATENCY_CAUSE_END (offsetof(struct latency_result, cause_footer) + 64)
#ifdef SUNSTONE_LATENCY_SERVICE_CYCLES
_Static_assert(offsetof(struct latency_result, service) == LATENCY_CAUSE_END, "service cycle section follows the cause footer");
#define LATENCY_SERVICE_END (offsetof(struct latency_result, service_footer) + 64)
#else
#define LATENCY_SERVICE_END LATENCY_CAUSE_END
#endif
#ifdef SUNSTONE_LATENCY_BAND_SWITCH
_Static_assert(offsetof(struct latency_result, bands) == LATENCY_SERVICE_END, "band section follows the footer before it");
#define LATENCY_BAND_END (offsetof(struct latency_result, band_footer) + 64)
#else
#define LATENCY_BAND_END LATENCY_SERVICE_END
#endif
#ifdef SUNSTONE_LATENCY_WAKE_SEGMENTS
_Static_assert(offsetof(struct latency_result, wake_segments) == LATENCY_BAND_END, "wake segment section follows the footer before it");
#define LATENCY_WAKE_END (offsetof(struct latency_result, wake_footer) + 64)
#else
#define LATENCY_WAKE_END LATENCY_BAND_END
#endif
_Static_assert(LATENCY_WAKE_END == sizeof(struct latency_result) && sizeof(struct latency_result) <= LATENCY_RESULT_LIMIT,
    "the cause sections end the result and fit the declared result region");
#endif

/* Prepare before scheduling. Run from a controller allowed on one core; all
 * workload tasks use fixed affinity and independently allocated static stacks. */
void latency_workload_init(void);
#ifndef SUNSTONE_LATENCY_MODEL
void latency_workload_bind(void *buffer);
#endif
void latency_workload_run(struct latency_result *);
void latency_workload_progress(void);
#ifdef SUNSTONE_LATENCY_MODEL
void latency_dma_model_tick(void);
void latency_dma_model_start(void);
unsigned latency_dma_model_stop(void);
void latency_dma_model_init(void);
#elif defined(SUNSTONE_LATENCY_DMA)
void latency_dma_native_start(void);
unsigned latency_dma_native_stop(void);
#endif

#endif
