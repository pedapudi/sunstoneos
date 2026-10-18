#ifndef SUNSTONE_A733_MEMORY_RESERVATIONS_H
#define SUNSTONE_A733_MEMORY_RESERVATIONS_H
#include "device_memory_pool.h"
#include "a733_dma_native.h"
/* The default profile reserves at most 2 MiB inside image RAM and validates it
 * once before concurrent users. The extended cached profile returns the same
 * boot singleton only after a733_device_memory_prepare() has validated both
 * complete linker reservations. Its mappings are privileged; EL0 clients need
 * an actual service interface. No backing writes occur during preparation.
 * All device users and remote firmware must already be excluded; mappings
 * remain pinned without aliases. Neither profile qualifies physical capacity,
 * remote exclusion, device reachability or bus completion. */
struct device_memory_pool *a733_memory_reservations_init(void);
/* Bind the existing native DMA operations to the exact linker-owned pool.
 * The native adapter and all controller/service state must lie outside it.
 * Performs translation/cache-geometry checks, but no peripheral MMIO. */
int a733_memory_dma_init(struct a733_dma_native *, struct device_memory_pool *,
                         uintptr_t registers, uint64_t register_physical,
                         const struct dma_fifo_port *, size_t,
                         void (*fatal)(void *), void *);
#endif
