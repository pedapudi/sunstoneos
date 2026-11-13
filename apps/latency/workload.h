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
 * and control profiles declare 131072 bytes, the cause profile 147456. */
#ifdef SUNSTONE_LATENCY_CAUSE
#define LATENCY_RESULT_LIMIT 147456U
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
_Static_assert(sizeof(struct latency_result) <= LATENCY_RESULT_LIMIT &&
    offsetof(struct latency_result, cause_footer) + 64 == sizeof(struct latency_result),
    "cause diagnostic and footer fit the declared result region");
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
