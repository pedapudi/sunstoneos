/* Combined producer/DMA model for the finite I2S stream core and the native DMA
 * controller. FIFO contents are synthetic; this fixture establishes ownership
 * and ordering, not physical audio or reset behavior. */
#include "a733_i2s_stream.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static struct a733_i2s_stream stream;
static struct a733_dma_controller dma;
static struct a733_i2s_stream_profile profile;
static struct a733_dma_transfer catalogue;
static struct a733_dma_mapping mappings[2];
static struct a733_dma_segment segments[2];
static _Alignas(64) unsigned char descriptors[128], samples[2][64], device_samples[64];
static uint32_t ir[0xa0/4], dr[0x2000/4];
static uint64_t now;
static unsigned acquired, released, writes, dma_started, invalidated;
static unsigned acquire_refused, release_refused, flush_stuck, health_result;
static unsigned pending_acquire, retained_acquire, late_resource, pending_release;
static uintptr_t failing_read, failing_write, ignored_write;
static unsigned cache_ticks, mapping_ticks;

enum { DMA_BASE = 0x04601000, DCH = 0x100 };
static unsigned ix(unsigned offset) { return (DCH + offset) / 4; }
static int owns(void) { return a733_dma_owns(&dma, &stream.active); }
static uint32_t dma_read(void *cookie, uintptr_t address)
{
    assert(cookie == &dma && address >= DMA_BASE && address < DMA_BASE + sizeof(dr));
    return dr[(address - DMA_BASE)/4];
}
static void dma_write(void *cookie, uintptr_t address, uint32_t value)
{
    assert(cookie == &dma && address >= DMA_BASE + DCH && address < DMA_BASE + DCH + 0x40);
    unsigned offset = address - DMA_BASE - DCH;
    assert(offset == 0 || offset == 4 || offset == 8 || offset == 0x34 || offset == 0x38);
    ++writes;
    /* The receiver may keep running, but its DMA request is always off when
     * the channel starts or stops. */
    if (offset == 0) assert(acquired && !(ir[28/4] & 8));
    if (offset == 0 && value == 1) { ++dma_started; dr[0x30/4] = 1; }
    if (offset == 0x38) dr[ix(offset)] &= ~value;
    else dr[ix(offset)] = value;
}
static void cache(void *cookie, enum a733_dma_cache op, void *cpu, size_t bytes)
{
    assert(cookie == &dma && bytes == 64);
    assert(cpu == samples[0] || cpu == samples[1] || cpu == descriptors || cpu == descriptors + 64);
    now += cache_ticks;
    if (op == A733_DMA_INVALIDATE) {
        assert((cpu == samples[0] || cpu == samples[1]) && !dr[ix(0)] && !dr[0x30/4]);
        assert(!(ir[28/4] & 8) && owns());
        ++invalidated;
        memcpy(cpu, device_samples, 64);
    }
}
static void barrier(void *cookie) { assert(cookie == &dma); }
static uint64_t ticks(void *cookie) { assert(cookie == &dma); return now; }
static int validate_mapping(void *cookie, const struct a733_dma_mapping *m)
{
    assert(cookie == &dma);
    now += mapping_ticks;
    for (unsigned i = 0; i < 2; ++i)
        if (m->bytes == 64 &&
            ((m->cpu == descriptors + i*64 && m->bus == 0x41000000u + i*64 && m->access == A733_DMA_DESCRIPTORS) ||
             (m->cpu == samples[i] && m->bus == 0x41000100u + i*64 && m->access == A733_DMA_WRITE))) return 1;
    return 0;
}
static int validate_fifo(void *cookie, const struct a733_dma_endpoint *e, enum a733_dma_access a)
{
    assert(cookie == &dma && acquired);
    return a == A733_DMA_READ && e->address == A733_I2S0_RXFIFO &&
           e->request == 3 && e->width == profile.format.container_bits / 8 && e->burst == 8;
}
static enum a733_i2s_resource_result acquire(void *cookie, const struct a733_i2s_plan *p, uint64_t deadline)
{
    assert(cookie == &stream && !acquired);
    if (acquire_refused) return A733_I2S_RESOURCE_BUSY;
    assert(p->format.module_hz == profile.format.module_hz);
    assert(p->pb_mask == ((1u<<5) | (1u<<6) | (1u<<8)));
    acquired = 1;
    if (late_resource) now = deadline;
    if (retained_acquire) return A733_I2S_RESOURCE_RETAINED;
    if (pending_acquire) return A733_I2S_RESOURCE_PENDING;
    return A733_I2S_RESOURCE_READY;
}
static enum a733_i2s_resource_result acquire_poll(void *cookie, uint64_t deadline)
{
    assert(cookie == &stream && acquired);
    if (late_resource) now = deadline;
    if (retained_acquire) return A733_I2S_RESOURCE_RETAINED;
    if (pending_acquire) { --pending_acquire; return A733_I2S_RESOURCE_BUSY; }
    return A733_I2S_RESOURCE_READY;
}
static enum a733_i2s_resource_result release(void *cookie, uint64_t deadline)
{
    assert(cookie == &stream && acquired && !(ir[0] & 7) && !ir[28/4]);
    assert(!dr[ix(0)] && !dr[0x30/4] && !owns());
    if (release_refused) return A733_I2S_RESOURCE_RETAINED;
    if (pending_release) { --pending_release; return A733_I2S_RESOURCE_PENDING; }
    if (late_resource) now = deadline;
    acquired = 0;
    ++released;
    return A733_I2S_RESOURCE_READY;
}
static enum a733_i2s_resource_result health(void *cookie, uint64_t deadline)
{
    assert(cookie == &stream && now < deadline);
    return health_result;
}
static int i2s_read(void *cookie, uintptr_t address, uint32_t *value)
{
    assert(cookie == &stream && acquired && address >= A733_I2S0_BASE &&
           address < A733_I2S0_BASE + sizeof(ir));
    if (address == failing_read) return 0;
    *value = ir[(address - A733_I2S0_BASE)/4];
    return 1;
}
/* Follows the verified-write contract in a733_i2s_stream.h. */
static int i2s_write(void *cookie, uintptr_t address, uint32_t value)
{
    assert(cookie == &stream && acquired && address >= A733_I2S0_BASE &&
           address < A733_I2S0_BASE + sizeof(ir));
    if (address == failing_write) return 0;
    unsigned offset = address - A733_I2S0_BASE;
    assert(offset != 16 && offset != 32); /* No FIFO data access. */
    assert(offset != 0 || !(value & (4u | 0xf00u))); /* No transmitter/output pins. */
    if (offset == 28 && (value & 8)) assert(dr[ix(0)] && (ir[0] & 1));
    ++writes;
    if (offset == 12) { ir[3] &= ~value; return 1; }
    uint32_t stored = value, ignore = offset == 20 ? 3u << 24 : 0;
    if (offset == 20 && !flush_stuck) stored &= ~ignore;
    if (address != ignored_write) ir[offset/4] = stored;
    return !((ir[offset/4] ^ value) & ~ignore);
}
static int now_us(void *cookie, uint64_t *out)
{ assert(cookie == &stream); *out = now; return 1; }
static void setup(unsigned bits, unsigned channels)
{
    /* A separate, independently reset model per case. */
    memset(&stream, 0, sizeof(stream));
    memset(&dma, 0, sizeof(dma));
    memset(ir, 0, sizeof(ir)); memset(dr, 0, sizeof(dr));
    memset(samples, 0xa5, sizeof(samples)); memset(descriptors, 0x5a, sizeof(descriptors));
    dr[0x34/4] = 1;
    dr[ix(8)] = 0x41001000;
    ir[20/4] = 0x120;
    now = acquired = released = writes = dma_started = invalidated = 0;
    acquire_refused = release_refused = flush_stuck = 0;
    pending_acquire = retained_acquire = late_resource = pending_release = 0;
    failing_read = failing_write = ignored_write = 0;
    cache_ticks = mapping_ticks = 0;
    health_result = A733_I2S_RESOURCE_READY;
    profile = (struct a733_i2s_stream_profile){{24576000, 16000, 0, bits, 32, channels, 0},
        {{acquire, acquire_poll, release, now_us, i2s_read, i2s_write}, health},
        &stream, A733_I2S_RECEIVE, 1000, 0};
    for (unsigned i = 0; i < 2; ++i) {
        mappings[i] = (struct a733_dma_mapping){samples[i], 0x41000100u + i*64, 64, A733_DMA_WRITE};
        segments[i] = (struct a733_dma_segment){{A733_I2S0_RXFIFO, 3, bits/8, 8},
                                               {0x41000100u + i*64, 0, bits/8, 8}, 64};
    }
    catalogue = (struct a733_dma_transfer){{descriptors, 0x41000000, 128, A733_DMA_DESCRIPTORS},
                                          segments, 2, mappings, 2};
    struct a733_dma_operations ops = {dma_read, dma_write, cache, barrier, ticks,
                                      validate_mapping, validate_fifo};
    assert(a733_dma_controller_init(&dma, &ops, &dma, DMA_BASE, 1, 10) == A733_DMA_CONTROL_OK);
}
static enum a733_i2s_status open_stream(void)
{ return a733_i2s_stream_open(&stream, &profile, &catalogue, &dma, 0, 10000); }
static void ready_stream(void)
{
    assert(open_stream() == A733_I2S_OPENING && !writes);
    assert(a733_i2s_stream_poll(&stream) == A733_I2S_READY);
    assert(ir[20/4] == 0x40075 && dma.channels[0].claimed);
}
static void start_stream(void)
{
    assert(a733_i2s_stream_start(&stream, 100) == A733_I2S_PREPARING);
    assert(!dma_started && (ir[0] & 1));
    assert(a733_i2s_stream_poll(&stream) == A733_I2S_ACTIVE && dma_started == 1);
    assert((ir[0] & 2) && ir[28/4] == 8);
}
/* Reap a stopped stream, then close it and release every resource. */
static void close_stream(void)
{
    assert(!owns() && a733_i2s_stream_reap(&stream) && stream.status == A733_I2S_COMPLETE);
    assert(a733_i2s_stream_close(&stream, 10000) == A733_I2S_CLOSED);
    assert(released == 1 && !acquired && !dma.channels[0].claimed);
}
static void drain(enum a733_i2s_status outcome)
{
    assert(a733_i2s_stream_poll(&stream) == A733_I2S_STOPPING && owns());
    dr[0x30/4] = 0;
    assert(a733_i2s_stream_poll(&stream) == outcome && !(ir[0] & 7) && !ir[28/4]);
}
static void held_recovery(void)
{
    assert(stream.status == A733_I2S_RECOVERY && acquired && stream.platform_held && stream.dma_held);
    unsigned old_writes = writes;
    assert(a733_i2s_stream_poll(&stream) == A733_I2S_RECOVERY);
    assert(a733_i2s_stream_cancel(&stream) == A733_I2S_RECOVERY);
    assert(a733_i2s_stream_close(&stream, 10000) == A733_I2S_INVALID);
    assert(writes == old_writes && !released && stream.status == A733_I2S_RECOVERY);
}
int main(void)
{
    /* Period completion: disable the DMA request first, keep the receiver and
     * the allocation until the delayed DMA drain, and preserve raw captured
     * bits, including 24-bit padding/sign patterns. */
    for (unsigned bits = 16; bits <= 32; bits += 16) {
        setup(bits, 2); ready_stream(); start_stream();
        for (unsigned i = 0; i < 64; ++i) device_samples[i] = (unsigned char)(i * 37u + 0x80);
        dr[ix(0x38)] = 4;
        assert(a733_i2s_stream_poll(&stream) == A733_I2S_ACTIVE);
        assert(owns() && !invalidated && samples[0][0] == 0xa5);
        assert((ir[0] & 2) && !ir[28/4]);
        dr[0x30/4] = 0;
        assert(a733_i2s_stream_poll(&stream) == A733_I2S_ACTIVE && invalidated == 1 && dma_started == 2);
        assert(!memcmp(samples[0], device_samples, 64) && ir[28/4] == 8);
        struct a733_i2s_period period;
        assert(a733_i2s_stream_get(&stream, &period) == 1 && period.kind == A733_I2S_CAPTURED);
        assert(period.slot == 0 && !period.first_frame && period.frames == 64 / (bits / 4));
        assert(a733_i2s_stream_cancel(&stream) == A733_I2S_STOPPING && !(ir[0] & 2));
        drain(A733_I2S_CANCELED);
        assert(!a733_i2s_stream_reap(&stream) && a733_i2s_stream_put(&stream, period.ticket) == 1);
        assert(a733_i2s_stream_put(&stream, period.ticket) == 0);
        close_stream();
        assert(a733_i2s_stream_start(&stream, 100) == A733_I2S_INVALID);
    }
    setup(32, 1); pending_acquire = 1;
    assert(open_stream() == A733_I2S_OPENING && !writes && stream.platform_held && !stream.dma_held);
    assert(a733_i2s_stream_poll(&stream) == A733_I2S_OPENING && !writes);
    assert(a733_i2s_stream_poll(&stream) == A733_I2S_READY);
    pending_release = 1;
    assert(a733_i2s_stream_close(&stream, 10000) == A733_I2S_CLOSING && stream.platform_held);
    assert(a733_i2s_stream_poll(&stream) == A733_I2S_CLOSED && !stream.platform_held && released);

    /* Retained, late and expired resource operations keep every resource. */
    for (unsigned late = 0; late < 2; ++late) {
        setup(32, 1); retained_acquire = !late; late_resource = late;
        assert(open_stream() == A733_I2S_RECOVERY);
        assert(stream.platform_held && !stream.dma_held && !writes);
        assert(a733_i2s_stream_poll(&stream) == A733_I2S_RECOVERY && !writes);
        assert(a733_i2s_stream_close(&stream, 20000) == A733_I2S_INVALID && !released);
    }
    setup(32, 1); pending_acquire = 1;
    assert(open_stream() == A733_I2S_OPENING);
    now = 10000;
    assert(a733_i2s_stream_poll(&stream) == A733_I2S_RECOVERY && stream.platform_held && !writes);
    setup(32, 1); ready_stream(); late_resource = 1;
    assert(a733_i2s_stream_close(&stream, 10000) == A733_I2S_RECOVERY && stream.platform_held);
    setup(32, 1); ready_stream(); release_refused = 1;
    assert(a733_i2s_stream_close(&stream, 10000) == A733_I2S_RECOVERY);
    assert(acquired && !stream.dma_held && !released);

    /* Busy platform authority refuses without device access. */
    setup(32, 1); acquire_refused = 1;
    assert(open_stream() == A733_I2S_CONFLICT && !writes && !acquired && !stream.self);
    setup(32, 1); ready_stream(); health_result = A733_I2S_RESOURCE_BUSY;
    unsigned old_writes = writes;
    assert(a733_i2s_stream_start(&stream, 100) == A733_I2S_CONFLICT && writes == old_writes);
    health_result = A733_I2S_RESOURCE_READY; stream.deadline = 0;
    assert(a733_i2s_stream_close(&stream, 10000) == A733_I2S_CLOSED && released == 1);
    /* A channel another owner claimed retains the platform resources. */
    setup(32, 1); assert(a733_dma_claim(&dma, 0) == A733_DMA_CONTROL_OK);
    assert(open_stream() == A733_I2S_OPENING);
    assert(a733_i2s_stream_poll(&stream) == A733_I2S_RECOVERY && !writes && acquired);

    /* Invalid catalogues are refused before any callback. */
    setup(32, 1); segments[1].source.request = 4;
    assert(open_stream() == A733_I2S_INVALID && !acquired && !stream.self);
    setup(32, 1); mappings[1] = mappings[0];
    assert(open_stream() == A733_I2S_INVALID && !acquired);
    /* A mapping the platform policy refuses fails the stream before DMA starts. */
    setup(32, 1); ready_stream(); mappings[0].bus += 128; segments[0].destination.address += 128;
    assert(a733_i2s_stream_start(&stream, 100) == A733_I2S_PREPARING);
    assert(a733_i2s_stream_poll(&stream) == A733_I2S_STOPPING && !owns());
    assert(a733_i2s_stream_poll(&stream) == A733_I2S_FAILED && !dma_started);
    close_stream();

    setup(32, 1); ready_stream(); flush_stuck = 1;
    assert(a733_i2s_stream_start(&stream, 100) == A733_I2S_PREPARING);
    assert(a733_i2s_stream_poll(&stream) == A733_I2S_PREPARING);
    now = 1000;
    assert(a733_i2s_stream_poll(&stream) == A733_I2S_RECOVERY && !dma_started && acquired);
    setup(32, 1); ready_stream();
    assert(a733_i2s_stream_start(&stream, 100) == A733_I2S_PREPARING);
    assert(a733_i2s_stream_cancel(&stream) == A733_I2S_STOPPING);
    assert(a733_i2s_stream_poll(&stream) == A733_I2S_CANCELED && !dma_started);
    close_stream();

    /* Cancellation, FIFO overrun, DMA fault and DMA deadline all disable the
     * receiver before draining; neither elapsed time nor a stop request
     * releases RAM. */
    for (unsigned fault = 0; fault < 4; ++fault) {
        setup(32, 1); ready_stream(); start_stream();
        if (fault == 0) assert(a733_i2s_stream_cancel(&stream) == A733_I2S_STOPPING);
        if (fault == 1) ir[12/4] = 2;
        if (fault == 2) dr[ix(0x38)] = 8;
        if (fault == 3) now = 100;
        if (fault) assert(a733_i2s_stream_poll(&stream) == A733_I2S_STOPPING);
        drain(fault ? A733_I2S_FAILED : A733_I2S_CANCELED);
        struct a733_i2s_period period;
        assert(a733_i2s_stream_get(&stream, &period) == 0); close_stream();
    }
    /* Uncertain states retain every resource and stop all device access. */
    setup(32, 1); ready_stream(); start_stream();
    assert(a733_i2s_stream_cancel(&stream) == A733_I2S_STOPPING); now = 10;
    assert(a733_i2s_stream_poll(&stream) == A733_I2S_RECOVERY && owns()); held_recovery();
    setup(32, 1); ready_stream(); start_stream(); failing_write = A733_I2S0_BASE + 28;
    assert(a733_i2s_stream_cancel(&stream) == A733_I2S_RECOVERY && owns()); held_recovery();
    setup(32, 1); ready_stream(); start_stream(); failing_read = A733_I2S0_BASE + 12;
    assert(a733_i2s_stream_poll(&stream) == A733_I2S_RECOVERY && owns()); held_recovery();
    setup(32, 1); ready_stream(); start_stream(); dr[0x34/4] = 0; dr[ix(0x38)] = 4;
    assert(a733_i2s_stream_poll(&stream) == A733_I2S_RECOVERY && owns()); held_recovery();
    assert(dma.channels[0].status == A733_DMA_RECOVERY && dr[ix(0x38)] == 4);
    setup(32, 1); ready_stream(); start_stream(); ignored_write = A733_I2S0_BASE + 28;
    assert(a733_i2s_stream_cancel(&stream) == A733_I2S_RECOVERY && owns()); held_recovery();
    setup(32, 1); ready_stream();
    assert(a733_i2s_stream_start(&stream, 100) == A733_I2S_PREPARING);
    failing_write = A733_I2S0_BASE + 28;
    assert(a733_i2s_stream_poll(&stream) == A733_I2S_RECOVERY && dma_started && owns()); held_recovery();
    /* Cache preparation that consumes the DMA deadline stops before enable. */
    setup(32, 1); ready_stream();
    assert(a733_i2s_stream_start(&stream, 100) == A733_I2S_PREPARING);
    cache_ticks = 60; mapping_ticks = 40;
    assert(a733_i2s_stream_poll(&stream) == A733_I2S_STOPPING && !dma_started);
    dr[0x30/4] = 0;
    assert(a733_i2s_stream_poll(&stream) == A733_I2S_FAILED); close_stream();
    puts("I2S stream/native DMA: completion, ordering, refusal, deadlines, and retained faults passed");
    return 0;
}
