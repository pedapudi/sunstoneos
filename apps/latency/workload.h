#ifndef SUNSTONE_LATENCY_WORKLOAD_H
#define SUNSTONE_LATENCY_WORKLOAD_H

#include "sunstone_latency.h"
#include <stddef.h>
#include <stdint.h>

#define LATENCY_CORES 8U
#define LATENCY_IDLE_SAMPLES 64U
#define LATENCY_BUSY_SAMPLES 128U
#define LATENCY_BYTES_PER_CORE (1024U * 1024U)
#define LATENCY_RESULT_MAGIC UINT64_C(0x53534c4154454e31)

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
    /* Counts inside the kind-0 sites above, same phase, core and slot. */
    struct ss_latency_site_pmu site_pmu[SS_LAT_PHASES + 1][LATENCY_CORES][SS_LAT_SITE_SLOTS];
#endif
};
_Static_assert(offsetof(struct latency_result, core) == 320 &&
    offsetof(struct latency_result, phases) == 5440 &&
    offsetof(struct latency_result, acquisitions) == 20224 &&
    offsetof(struct latency_result, wakes) == 20848 &&
    offsetof(struct latency_result, sites) == 57728 &&
#ifdef SUNSTONE_LATENCY_PMU
    sizeof(struct latency_result) == 63872 + 1536,
#else
    sizeof(struct latency_result) == 63872,
#endif
    "latency binary report layout");

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
