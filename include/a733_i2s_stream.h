#ifndef SUNSTONE_A733_I2S_STREAM_H
#define SUNSTONE_A733_I2S_STREAM_H
#include "a733_i2s.h"

#define A733_I2S_STREAM_PERIODS 8u
#define A733_I2S0_TXFIFO (A733_I2S0_BASE + 0x20u)
enum a733_i2s_route { A733_I2S_ROUTE_I2S0, A733_I2S_ROUTE_HDMI3 };
enum a733_i2s_direction { A733_I2S_RECEIVE, A733_I2S_TRANSMIT };
enum a733_i2s_period_kind { A733_I2S_NO_PERIOD, A733_I2S_CAPTURED,
    A733_I2S_TX_FILL, A733_I2S_TX_MEMORY_CONSUMED, A733_I2S_TX_REPEAT_PROGRESS, A733_I2S_CAPTURED_COPY };
/* Copied observation, never a wire timestamp. RX counter counts samples entering
 * RX FIFO; TX counter counts writes into TX FIFO. CPU access begins only after
 * GET succeeds and ends before PUT is queued. TX consumption does not prove
 * that the last sample has left the serializer or reached a loudspeaker. */
struct a733_i2s_period {
    uint64_t ticket, first_frame, observed_before_us, observed_after_us;
    uint32_t counter;
    unsigned slot, frames;
    enum a733_i2s_period_kind kind;
};
struct a733_i2s_stream_operations {
    /* io.write succeeds only after reading the register back under the same
     * register lock and matching every written bit, except ISTA (write-one-
     * to-clear) and the self-clearing FCTL flush bits. A mismatch retains
     * failure. The stream therefore never reads a written value back. */
    struct a733_i2s_operations io;
    /* Authenticate actual clock and pin leases, without holding any device
     * register mutex. READY proves live authority; BUSY has no device effects.
     * RETAINED or a late result prohibits subsequent device access. */
    enum a733_i2s_resource_result (*health)(void *, uint64_t deadline_us);
};
struct a733_i2s_stream_profile {
    struct a733_i2s_format format;
    struct a733_i2s_stream_operations operations;
    void *cookie;
    enum a733_i2s_direction direction;
    uint64_t operation_us; /* Finite health/register operation budget. */
    unsigned cyclic; /* Fixed private ring; live RX is checked-copy only. */
    enum a733_i2s_route route; /* Zero preserves I2S0; HDMI3 is fixed finite TX. */
};
/* Catalogue: 2..8 equal whole-frame segments, one disjoint full allocation per
 * period, and one separate 64-byte descriptor slot per period. All metadata is
 * private, immutable and platform-lived. TX source/RX destination must equal
 * its mapping base; segments may use a prefix of their allocation. */
int a733_i2s_stream_valid(const struct a733_i2s_stream_profile *,
    const struct a733_dma_transfer *);
struct a733_i2s_stream_slot {
    struct a733_i2s_period result;
    unsigned state;
};
/* Private zero-initialized storage; exactly one serialized worker owns this
 * object, its controller and callbacks. No copied objects or independent channel
 * polling. Only a clean CLOSED session may be reset for another open; preserve
 * its generation sequence. Parent allocations remain reserved for lifetime.
 * Failure retains every resource; there is no automatic reset/retry. */
struct a733_i2s_stream {
    struct a733_i2s_stream *self, *peer;
    unsigned duplex;
    struct a733_i2s_stream_profile profile;
    struct a733_i2s_plan plan;
    struct a733_dma_controller *dma;
    const struct a733_dma_transfer *catalogue;
    struct a733_dma_transfer active;
    struct a733_dma_segment active_segment; /* Finite private prefix, immutable while DMA owns it. */
    struct a733_i2s_stream_slot slots[A733_I2S_STREAM_PERIODS];
    uint64_t deadline, time, timeout, generation, frames, bridge_deadline;
    enum a733_i2s_status status, outcome;
    unsigned channel, count, current, next, stage, dma_held, platform_held, priming;
    uint64_t reported, raw_next, cyclic_since, cyclic_observed_us;
    struct a733_i2s_period snapshot_result;
    unsigned snapshot_state, cyclic_started;
    unsigned input_ended, end_slot, end_frames;
    uint64_t fifo_deadline, bridge_until;
    unsigned bridge_polls, bridge_passes;
    _Alignas(64) unsigned char snapshot[4096];
};
enum a733_i2s_status a733_i2s_stream_open(struct a733_i2s_stream *,
    const struct a733_i2s_stream_profile *, const struct a733_dma_transfer *,
    struct a733_dma_controller *, unsigned channel, uint64_t deadline_us);
/* The serialized service joins exactly one opposite direction. The first open
 * acquires both data pins; the second shares that live binding. Both catalogues
 * and stream objects remain immutable and allocated until both closes finish. */
enum a733_i2s_status a733_i2s_stream_duplex_open(struct a733_i2s_stream *,
    const struct a733_i2s_stream_profile *,const struct a733_dma_transfer *,
    struct a733_dma_controller *,unsigned,uint64_t,struct a733_i2s_stream *);
/* TX requires every catalogue period committed by PUT, or every period up to
 * and including a committed END. After START, GET leases only consumed periods. */
enum a733_i2s_status a733_i2s_stream_start(struct a733_i2s_stream *, uint64_t period_timeout_ticks);
enum a733_i2s_status a733_i2s_stream_poll(struct a733_i2s_stream *);
enum a733_i2s_status a733_i2s_stream_cancel(struct a733_i2s_stream *);
enum a733_i2s_status a733_i2s_stream_close(struct a733_i2s_stream *, uint64_t deadline_us);
/* GET is a live-authority operation and changes output only on success.
 * Cyclic TX progress is metadata only and grants no PUT lease.
 * PUT consumes the exact ticket once. RX returns an allocation; TX commits
 * immutable payload. On a stopped stream PUT relinquishes without restarting.
 * Return: 1 success, 0 unavailable, -1 retained fault. */
int a733_i2s_stream_get(struct a733_i2s_stream *, struct a733_i2s_period *);
/* Read-only CPU staging lease after CAPTURED_COPY GET, never raw DMA memory. */
int a733_i2s_stream_copy(const struct a733_i2s_stream *,uint64_t,void *,size_t);
int a733_i2s_stream_put(struct a733_i2s_stream *, uint64_t ticket);
/* Commit exactly the final finite TX lease. valid_frames is nonzero and at most
 * this catalogue period's capacity. No later PUT commits payload; those PUTs
 * return completed/unused leases only. INPUT_ENDED proves DMA ownership ended
 * and FIFO empty was observed, not serializer completion. TX/clock/pin/catalogue
 * leases remain held; REAP/CLOSE require explicit potentially truncating cancel. */
int a733_i2s_stream_end(struct a733_i2s_stream *,uint64_t ticket,uint64_t valid_frames);
/* Terminal REAP is allowed only after actual drain and every client PUT;
 * discards unclaimed periods, but preserves the whole catalogue reservation. */
int a733_i2s_stream_reap(struct a733_i2s_stream *);
#endif
