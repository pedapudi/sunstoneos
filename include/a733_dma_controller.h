#ifndef SUNSTONE_A733_DMA_CONTROLLER_H
#define SUNSTONE_A733_DMA_CONTROLLER_H

#include "a733_dma.h"
#include "a733_range.h"
#include "dma_address_width.h"

#define A733_DMA_CHANNELS 16u
#define A733_DMA_MAX_MAPPINGS (2u * A733_DMA_MAX_SEGMENTS)

enum a733_dma_access { A733_DMA_READ, A733_DMA_WRITE, A733_DMA_DESCRIPTORS };
enum a733_dma_cache { A733_DMA_CLEAN, A733_DMA_CLEAN_INVALIDATE, A733_DMA_INVALIDATE };
enum a733_dma_control_status {
    A733_DMA_CONTROL_OK,
    A733_DMA_ACTIVE,
    A733_DMA_STOPPING,
    A733_DMA_COMPLETE,
    A733_DMA_CANCELED,
    A733_DMA_FAILED,
    A733_DMA_INVALID = -1,
    A733_DMA_CONFLICT = -2,
    A733_DMA_UNAVAILABLE = -3,
    A733_DMA_RECOVERY = -4
};

/* CPU and bus bases and size must be 64-byte aligned. Reserve the entire
 * allocation for DMA until completion, including unused bytes in its lines.
 * Platform validation must prove a canonical, stable mapping of ordinary RAM. */
struct a733_dma_mapping {
    void *cpu;
    uint64_t bus;
    size_t bytes;
    enum a733_dma_access access;
};

/* Shape every DMA0 mapping needs: nonempty 64-byte-aligned CPU and bus ranges,
 * no CPU address wrap, and a bus range below A733_DMA0_DATA_LIMIT (2^35). */
static inline int a733_dma_mapping_shape(const struct a733_dma_mapping *m)
{
    return m->cpu && m->bytes && !((uintptr_t)m->cpu & 63) && !(m->bus & 63) &&
        !(m->bytes & 63) && (uintptr_t)m->cpu <= UINTPTR_MAX - m->bytes &&
        m->bus < A733_DMA0_DATA_LIMIT && m->bytes <= A733_DMA0_DATA_LIMIT - m->bus;
}
/* Either the CPU or the bus ranges intersect; both mappings have valid shape. */
static inline int a733_dma_mappings_overlap(const struct a733_dma_mapping *a,
                                            const struct a733_dma_mapping *b)
{
    return a733_extents_overlap_prechecked((uintptr_t)a->cpu, a->bytes, (uintptr_t)b->cpu, b->bytes) ||
           a733_extents_overlap_prechecked(a->bus, a->bytes, b->bus, b->bytes);
}

struct a733_dma_transfer {
    struct a733_dma_mapping descriptors;
    const struct a733_dma_segment *segments;
    size_t count;
    const struct a733_dma_mapping *mappings;
    size_t mapping_count;
};

/* All callbacks execute synchronously under the caller's serialization.
 * They must not reenter the controller. Cache callbacks maintain the entire
 * supplied allocation; barrier orders CPU caches, memory, and device accesses.
 * validate_mapping is read-only and must reject reserved RAM, unstable or
 * aliased mappings, and cache lines wider than the ownership granule (64).
 * ticks is a monotonic wrapping 64-bit counter; durations must be below 2^63.
 * Hardware watchdog protection is required because MMIO itself can stall. */
struct a733_dma_operations {
    uint32_t (*read)(void *cookie, uintptr_t address);
    void (*write)(void *cookie, uintptr_t address, uint32_t value);
    void (*cache)(void *cookie, enum a733_dma_cache operation,
                  void *cpu, size_t bytes);
    void (*barrier)(void *cookie);
    uint64_t (*ticks)(void *cookie);
    int (*validate_mapping)(void *cookie, const struct a733_dma_mapping *mapping);
    /* Validate request routing, address, width, and burst for this peripheral.
     * READ consumes a peripheral FIFO; WRITE supplies a peripheral FIFO. */
    int (*validate_fifo)(void *cookie, const struct a733_dma_endpoint *endpoint,
                         enum a733_dma_access access);
};

/* Implementation state; callers must not modify initialized fields. */
struct a733_dma_channel {
    const struct a733_dma_transfer *transfer;
    uint64_t started, timeout, stop_started;
    uint32_t saved_descriptor;
    enum a733_dma_control_status status, outcome;
    unsigned claimed, cyclic;
    uint32_t package_last;
    uint64_t packages, observed;
};
struct a733_dma_controller {
    struct a733_dma_operations operations;
    void *cookie;
    uintptr_t base;
    uint64_t stop_timeout;
    uint16_t authorized_channels;
    unsigned initialized;
    struct a733_dma_channel channels[A733_DMA_CHANNELS];
    /* Interrupt-enabled pending events acknowledged by poll; wraps. */
    uint32_t events;
};

/* Zero-initialize controller once. Caller must serialize all methods and
 * maintain an exclusive firmware/platform lease for authorized_channels.
 * Before claim: clocks/reset/autogating must be prepared. The controller never
 * touches the GIC; the DMA interrupt only wakes the caller, which still polls.
 * Global routing is never changed.
 * No reinitialization or forced release of a failed channel is provided. */
enum a733_dma_control_status a733_dma_controller_init(
    struct a733_dma_controller *controller, const struct a733_dma_operations *ops,
    void *cookie, uintptr_t base, uint16_t authorized_channels, uint64_t stop_timeout);
enum a733_dma_control_status a733_dma_claim(struct a733_dma_controller *, unsigned channel);
enum a733_dma_control_status a733_dma_release(struct a733_dma_controller *, unsigned channel);

/* Transfer metadata, descriptor storage, mappings, and buffers must remain
 * immutable/exclusively leased while owns() is true. Read-only source data may
 * be shared. No CPU writes to any leased cache line are permitted. INVALID or
 * CONFLICT leaves descriptors unchanged. Submission can return STOPPING when
 * configuration readback fails; poll until terminal or recovery status.
 * COMPLETE/CANCELED/FAILED release buffers after verified quiescence.
 * RECOVERY retains them indefinitely if owns() is true; independent reset is
 * required. Poll is bounded and never waits internally. */
enum a733_dma_control_status a733_dma_submit(
    struct a733_dma_controller *, unsigned channel,
    const struct a733_dma_transfer *, uint64_t timeout);
/* Immutable memory-to-FIFO only. The caller keeps FIFO requests disabled until
 * submit returns ACTIVE; every mapping is DMA_READ. No live CPU payload lease.
 * timeout bounds progress observations and a missing completed package. */
enum a733_dma_control_status a733_dma_submit_cyclic_tx(struct a733_dma_controller *,
    unsigned,const struct a733_dma_transfer *,uint64_t);
enum a733_dma_control_status a733_dma_submit_cyclic_rx(struct a733_dma_controller *,
    unsigned,const struct a733_dma_transfer *,uint64_t);
int a733_dma_cyclic_progress(const struct a733_dma_controller *,unsigned,uint64_t *);
/* Copy only one completed, not-yet-reused RX range into private staging. No
 * raw lease escapes. Period k is copied only while period+2 <= packages <=
 * period+count-2, so a ring needs at least four periods. Return1 verified
 * copy,0 not yet stable/ready,-1 overrun or ambiguous history. A failed copy
 * remains unpublished. Serialized worker only. */
int a733_dma_cyclic_rx_copy(struct a733_dma_controller *,unsigned,uint64_t,
    void *,size_t);
/* Observe the channel's finite transfer t without acknowledging PENDING, so a
 * FIFO producer can disable its requests before poll() stops the channel.
 * ACTIVE: still running. COMPLETE: queue end observed. FAILED: error event or
 * expired timeout; the caller still stops the channel. RECOVERY: routing, mode
 * or high-address state is inconsistent and the channel is retained as poll()
 * retains it. INVALID: t is not this channel's active finite transfer. */
enum a733_dma_control_status a733_dma_finite_state(struct a733_dma_controller *, unsigned channel,
                                                   const struct a733_dma_transfer *t);
enum a733_dma_control_status a733_dma_poll(struct a733_dma_controller *, unsigned channel);
enum a733_dma_control_status a733_dma_cancel(struct a733_dma_controller *, unsigned channel);
/* 1 when the channel is ACTIVE, so only an enabled interrupt event or its
 * deadline can advance it, and that deadline is more than margin ticks away.
 * The caller may then wait for the interrupt instead of polling each tick. */
int a733_dma_event_wait(struct a733_dma_controller *, unsigned channel, uint64_t margin);
int a733_dma_owns(const struct a733_dma_controller *, const struct a733_dma_transfer *);

#endif
