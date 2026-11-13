#ifndef SUNSTONE_DMA_EVENT_REPORT_H
#define SUNSTONE_DMA_EVENT_REPORT_H
#include <stdint.h>
#include "sunstone_dma.h"
enum { DMA_EVENT_IDLE_US=1000000u,DMA_EVENT_JOBS=2u };
struct dma_event_result {
    uint64_t magic,version,idle_begin_us,idle_end_us,jobs,compared_bytes;
    struct ss_dma_owner_stats before,after,stopped;
    uint64_t job_irqs[DMA_EVENT_JOBS],job_deadline_expiries[DMA_EVENT_JOBS];
};
#endif
