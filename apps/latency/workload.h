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
struct latency_result {
    uint64_t magic, frequency, elapsed_ticks;
    uint32_t core_mask, model, modeled_dma_jobs, timer_irq_core_mask;
    struct latency_traffic traffic[LATENCY_CORES];
    struct ss_latency_core core[LATENCY_CORES];
};
_Static_assert(offsetof(struct latency_result, core) == 320 &&
    sizeof(struct latency_result) == 5440, "latency binary report layout");

/* Prepare before scheduling. Run from a controller allowed on one core; all
 * workload tasks use fixed affinity and independently allocated static stacks. */
void latency_workload_init(void);
void latency_workload_run(struct latency_result *);
void latency_workload_progress(void);
#ifdef SUNSTONE_LATENCY_MODEL
void latency_dma_model_tick(void);
void latency_dma_model_start(void);
unsigned latency_dma_model_stop(void);
void latency_dma_model_init(void);
#endif

#endif
