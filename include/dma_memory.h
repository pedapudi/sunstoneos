#ifndef SUNSTONE_DMA_MEMORY_H
#define SUNSTONE_DMA_MEMORY_H
#include "a733_dma_controller.h"

#define DMA_MEMORY_MAX_WINDOWS 16u
#define DMA_MEMORY_MAX_PORTS 64u
#define DMA_MEMORY_ROLE(access) (1u << (access))

struct dma_memory_window {
    uintptr_t cpu;
    uint64_t physical;
    size_t bytes;
    unsigned roles;
};
struct dma_fifo_port {
    uint64_t address;
    unsigned request, widths, bursts;
    enum a733_dma_access access;
};
struct dma_translation {
    uint64_t physical;
    unsigned attributes, shareability;
};
typedef int (*dma_translate_fn)(void *, uintptr_t, int write, struct dma_translation *);

/* Platform-owned registry, immutable after initialization. Windows must contain
 * only dedicated, pinned DMA RAM. Bus addresses equal physical addresses;
 * IOMMU remapping and aliases are not supported. No allocation is performed.
 * CPU mappings must remain stable while any channel owns their buffers.
 * The translator must check the requested CPU access without changing mappings.
 * All participating CPUs must support the controller's 64-byte ownership unit. */
struct dma_memory_policy {
    struct dma_memory_window windows[DMA_MEMORY_MAX_WINDOWS];
    struct dma_fifo_port ports[DMA_MEMORY_MAX_PORTS];
    size_t window_count, port_count;
    dma_translate_fn translate;
    void *cookie;
    unsigned initialized;
};

/* Zero-initialize once. Failure leaves the policy unchanged. FIFO width/burst
 * masks use bit N for N bytes/accesses. Tables are copied on success. */
int dma_memory_init(struct dma_memory_policy *, const struct dma_memory_window *, size_t,
                    const struct dma_fifo_port *, size_t, dma_translate_fn, void *cookie);
int dma_memory_accept(const struct dma_memory_policy *, const struct a733_dma_mapping *);
int dma_memory_fifo(const struct dma_memory_policy *, const struct a733_dma_endpoint *,
                    enum a733_dma_access);
#endif
