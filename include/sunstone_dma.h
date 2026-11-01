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
#endif
