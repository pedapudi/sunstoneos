#ifndef SUNSTONE_A733_DMA_H
#define SUNSTONE_A733_DMA_H

#include <stddef.h>
#include <stdint.h>

#define A733_DMA_DESCRIPTOR_BYTES 64u
#define A733_DMA_MAX_SEGMENTS 64u
/* Software policy pending physical length qualification; not a silicon limit. */
#define A733_DMA_MAX_SEGMENT_BYTES (1024u * 1024u)

struct a733_dma_endpoint {
    uint64_t address;       /* DMA bus address, never an inferred CPU pointer. */
    unsigned request;      /* 0: incrementing memory; 1..63: fixed FIFO port. */
    unsigned width;        /* Bytes per bus access: 1, 2, or 4. */
    unsigned burst;        /* Bus accesses per burst: 1, 4, 8, or 16. */
};

struct a733_dma_segment {
    struct a733_dma_endpoint source, destination;
    uint32_t bytes;
};

/* One private 64-byte cache line per hardware descriptor. Align the allocation
 * to 64 bytes. Only the first 24 bytes contain hardware fields. */
struct a733_dma_descriptor {
    uint8_t bytes[A733_DMA_DESCRIPTOR_BYTES];
};

enum a733_dma_result {
    A733_DMA_OK,
    A733_DMA_ARGUMENT,
    A733_DMA_FORMAT,
    A733_DMA_ADDRESS,
    A733_DMA_ALIGNMENT,
    A733_DMA_ALIAS
};

/* Build a finite chain without MMIO, allocation, or cache operations.
 * On error, output is unchanged. Capacity is in descriptor slots.
 * Input and output must remain exclusively accessible during this call.
 *
 * Data addresses fit 35 bits. Descriptor addresses must be below 0xfffff800;
 * wider descriptor addressing is unresolved. Source memory may be shared
 * among segments; destinations must not overlap any source or destination.
 * Fixed FIFO ports may be reused. Memory-to-memory, memory-to-FIFO, and
 * FIFO-to-memory segments are accepted. FIFO-to-FIFO transfers are rejected.
 *
 * This encoder does not establish DMA ownership or validate RAM mappings,
 * peripheral request IDs, bus reachability, or cache coherence. A controller
 * service must do so before execution and retain mappings and buffers until
 * hardware quiescence is verified. See docs/dma.md. No hardware is started.
 */
enum a733_dma_result a733_dma_build(
    struct a733_dma_descriptor *output, size_t capacity, uint64_t descriptor_bus,
    const struct a733_dma_segment *segments, size_t count);

/* Concrete immutable playback ring. Same validation/atomic failure contract;
 * every segment reads RAM and writes the same FIFO, with equal payload sizes. */
enum a733_dma_result a733_dma_build_cyclic(struct a733_dma_descriptor *,size_t,
    uint64_t,const struct a733_dma_segment *,size_t);
#endif
