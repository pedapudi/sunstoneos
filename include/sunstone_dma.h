#ifndef SUNSTONE_DMA_H
#define SUNSTONE_DMA_H
#include <stdint.h>
/* Fixed RAM-copy catalogue supplied by the selected OS service. Source and
 * destination remain reserved until return. CPU access is allowed only before
 * SUBMIT or after COMPLETE, and ends before REAP. All calls are nonblocking:
 * 1 completes, 0 is pending, -1 is a retained failure. */
struct ss_dma_buffers { void *source, *destination; uint32_t bytes, reserved; };
int ss_dma_open(struct ss_dma_buffers *out);
int ss_dma_submit(void);
int ss_dma_complete(void);
int ss_dma_reap(void);
int ss_dma_close(void);
int ss_dma_retire(void);
/* Advisory owner counters, sampled independently with atomic loads. They do
 * not grant buffers or prove hardware completion. waiting is the worker's
 * indefinite-wait intent; retired is the complete class retirement proof,
 * including its pool bootstrap, not only the service worker's stop.
 * The registered application task may query a constructed service after
 * startup is ready or failed, including after retirement and before quiesce
 * is requested.
 * Returns 1 copied, 0 refused; refusal leaves out unchanged. No device I/O. */
struct ss_dma_owner_stats {
    uint64_t waits,wakes,idle_us,idle_notifications,idle_timeouts,deadline_expiries,irqs;
    uint32_t waiting,retired;
};
#ifdef __cplusplus
static_assert(sizeof(struct ss_dma_owner_stats)==64,"DMA owner statistics geometry");
#else
_Static_assert(sizeof(struct ss_dma_owner_stats)==64,"DMA owner statistics geometry");
#endif
int ss_dma_owner_stats(struct ss_dma_owner_stats *out);
#endif
