#ifndef SUNSTONE_A733_DMA_RESERVED_H
#define SUNSTONE_A733_DMA_RESERVED_H
#include "device_memory_pool.h"
#include "a733_dma_controller.h"

#define A733_DMA_RESERVED_MAPPINGS (DEVICE_MEMORY_MAX_ALLOCATIONS - 1u)
struct a733_dma_reserved_buffer {
    struct device_memory_lease lease;
    enum a733_dma_access access;
};
/* One serialized channel service. Keep this object and the controller private,
 * outside the pool. Do not call the controller directly after binding, mutate
 * its callbacks or reset pool/controller state. Hold the pool's common lock
 * throughout submission/poll/cancel and all allocation operations. */
struct a733_dma_reserved {
    uintptr_t self;
    struct {
        struct device_memory_pool *pool;
        struct a733_dma_controller *controller;
        uint64_t owner;
        unsigned channel;
    } binding;
    struct device_memory_pool *pool;
    struct a733_dma_controller *controller;
    uint64_t owner;
    unsigned channel, initialized, active, retained;
    size_t lease_count;
    struct device_memory_lease leases[DEVICE_MEMORY_MAX_ALLOCATIONS];
    struct a733_dma_transfer transfer;
    struct a733_dma_segment segments[A733_DMA_MAX_SEGMENTS];
    struct a733_dma_mapping mappings[A733_DMA_RESERVED_MAPPINGS];
};
/* Controller must already be initialized and channel claimed with no transfer.
 * Binding performs no MMIO. Native callers first use a733_memory_dma_init and
 * the existing native operations. Physical clock, routing, remote-owner,
 * watchdog and DMA completion guarantees remain prerequisites. */
int a733_dma_reserved_init(struct a733_dma_reserved *, struct device_memory_pool *,
                           struct a733_dma_controller *, unsigned channel, uint64_t owner);
/* Copy caller metadata; each buffer is an entire live allocation of this owner.
 * The complete rounded extent remains leased and undergoes cache maintenance,
 * including padding beyond the bytes touched by the transfer. */
enum a733_dma_control_status a733_dma_reserved_submit(struct a733_dma_reserved *,
    struct device_memory_lease descriptors, const struct a733_dma_reserved_buffer *, size_t,
    const struct a733_dma_segment *, size_t, uint64_t timeout);
enum a733_dma_control_status a733_dma_reserved_poll(struct a733_dma_reserved *);
enum a733_dma_control_status a733_dma_reserved_cancel(struct a733_dma_reserved *);
#endif
