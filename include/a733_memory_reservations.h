#ifndef SUNSTONE_A733_MEMORY_RESERVATIONS_H
#define SUNSTONE_A733_MEMORY_RESERVATIONS_H
#include "device_memory_pool.h"
#include "a733_dma_native.h"
/* Single-core A733 image only. Optional __device_pool_size is a multiple of
 * 4096, at most 1 MiB. Linker owns the entire range inside its 15 MiB image.
 * Call once at EL1 after platform_init and before concurrent users. No writes
 * to pool backing occur here. All device users and remote firmware must already
 * be excluded; stage-1 identity mappings must remain pinned without aliases.
 * Does not qualify physical capacity, remote exclusion, or bus completion. */
struct device_memory_pool *a733_memory_reservations_init(void);
/* Bind the existing native DMA operations to the exact linker-owned pool.
 * The native adapter and all controller/service state must lie outside it.
 * Performs translation/cache-geometry checks, but no peripheral MMIO. */
int a733_memory_dma_init(struct a733_dma_native *, struct device_memory_pool *,
                         uintptr_t registers, uint64_t register_physical,
                         const struct dma_fifo_port *, size_t,
                         void (*fatal)(void *), void *);
#endif
