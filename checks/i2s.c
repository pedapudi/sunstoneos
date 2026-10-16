/* Combined producer/DMA model. The FIFO contents are synthetic; this fixture
 * establishes ownership and ordering, not physical audio or reset behavior. */
#include "a733_i2s.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static struct a733_i2s_capture capture;
static struct a733_dma_controller dma;
static struct a733_dma_transfer transfer;
static struct a733_dma_mapping mapping;
static struct a733_dma_segment segment;
static _Alignas(64) unsigned char descriptors[64], samples[64], device_samples[64];
static uint32_t ir[0xa0/4], dr[0x2000/4];
static uint64_t now;
static unsigned acquired, released, writes, dma_started, invalidated;
static unsigned acquire_refused, release_refused, flush_stuck;
static unsigned pending_acquire, retained_acquire, late_resource, pending_release;
static uintptr_t failing_read, failing_write, ignored_write;
static unsigned cache_ticks, mapping_ticks;
static struct a733_i2s_format format;

enum { DMA_BASE = 0x04601000, DCH = 0x100 };
static unsigned ix(unsigned offset) { return (DCH + offset) / 4; }
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
    if (offset == 0 && value == 1) {
        assert(acquired && !(ir[0] & 2) && !(ir[28/4] & 8));
        ++dma_started;
        dr[0x30/4] = 1;
    }
    /* Every DMA disable must follow checked producer shutdown. */
    if (offset == 0 && value == 0) assert(!(ir[0] & 2) && ir[28/4] == 0);
    if (offset == 0x38) dr[ix(offset)] &= ~value;
    else dr[ix(offset)] = value;
}
static void cache(void *cookie, enum a733_dma_cache op, void *cpu, size_t bytes)
{
    assert(cookie == &dma && bytes == 64);
    assert(cpu == samples || cpu == descriptors);
    now += cache_ticks;
    if (op == A733_DMA_INVALIDATE) {
        assert(cpu == samples && !dr[ix(0)] && !dr[0x30/4]);
        assert(!(ir[0] & 2) && ir[28/4] == 0);
        assert(a733_i2s_owns(&capture));
        ++invalidated;
        memcpy(samples, device_samples, 64);
    }
}
static void barrier(void *cookie) { assert(cookie == &dma); }
static uint64_t ticks(void *cookie) { assert(cookie == &dma); return now; }
static int validate_mapping(void *cookie, const struct a733_dma_mapping *m)
{
    assert(cookie == &dma);
    now += mapping_ticks;
    return m->bytes == 64 &&
        ((m->cpu == descriptors && m->bus == 0x41000000 && m->access == A733_DMA_DESCRIPTORS) ||
         (m->cpu == samples && m->bus == 0x41000100 && m->access == A733_DMA_WRITE));
}
static int validate_fifo(void *cookie, const struct a733_dma_endpoint *e, enum a733_dma_access a)
{
    assert(cookie == &dma && acquired);
    return a == A733_DMA_READ && e->address == A733_I2S0_RXFIFO &&
           e->request == 3 && e->width == format.container_bits / 8 && e->burst == 8;
}
static enum a733_i2s_resource_result acquire(void *cookie, const struct a733_i2s_plan *p, uint64_t deadline)
{
    assert(cookie == &capture && !acquired);
    (void)deadline;
    if (acquire_refused) return A733_I2S_RESOURCE_BUSY;
    assert(p->format.module_hz == format.module_hz);
    assert(p->pb_mask == ((1u<<5) | (1u<<6) | (1u<<8) | (format.mclk_hz ? 1u<<4 : 0)));
    acquired = 1;
    if (late_resource) now=deadline;
    if (retained_acquire) return A733_I2S_RESOURCE_RETAINED;
    if (pending_acquire) return A733_I2S_RESOURCE_PENDING;
    return A733_I2S_RESOURCE_READY;
}
static enum a733_i2s_resource_result release(void *cookie, uint64_t deadline)
{
    assert(cookie == &capture && acquired && !(ir[0] & 7) && !ir[28/4]);
    assert(!dr[ix(0)] && !dr[0x30/4] && !a733_i2s_owns(&capture));
    (void)deadline;
    if (release_refused) return A733_I2S_RESOURCE_RETAINED;
    if (pending_release) { --pending_release; return A733_I2S_RESOURCE_PENDING; }
    if (late_resource) now=deadline;
    acquired = 0;
    ++released;
    return A733_I2S_RESOURCE_READY;
}
static int i2s_read(void *cookie, uintptr_t address, uint32_t *value)
{
    assert(cookie == &capture && acquired && address >= A733_I2S0_BASE &&
           address < A733_I2S0_BASE + sizeof(ir));
    if (address == failing_read) return 0;
    *value = ir[(address - A733_I2S0_BASE)/4];
    return 1;
}
static int i2s_write(void *cookie, uintptr_t address, uint32_t value)
{
    assert(cookie == &capture && acquired && address >= A733_I2S0_BASE &&
           address < A733_I2S0_BASE + sizeof(ir));
    if (address == failing_write) return 0;
    if (address == ignored_write) return 1;
    unsigned offset = address - A733_I2S0_BASE;
    assert(offset != 12 && offset != 16 && offset != 32); /* No status ACK or FIFO I/O. */
    assert(offset != 0 || !(value & (4u | 0xf00u))); /* No transmitter/output pins. */
    if (offset == 28 && value == 8) assert(dr[ix(0)] && (ir[0] & 2));
    ++writes;
    if (offset == 20 && !flush_stuck) value &= ~(1u << 24);
    ir[offset/4] = value;
    return 1;
}
static enum a733_i2s_resource_result acquire_poll(void *cookie, uint64_t deadline)
{
    assert(cookie == &capture && acquired);
    if (late_resource) now=deadline;
    if (retained_acquire) return A733_I2S_RESOURCE_RETAINED;
    if (pending_acquire) { --pending_acquire; return A733_I2S_RESOURCE_BUSY; }
    return A733_I2S_RESOURCE_READY;
}
static int now_us(void *cookie, uint64_t *out)
{ assert(cookie == &capture); *out = now; return 1; }
static const struct a733_i2s_operations iops = {
    acquire, acquire_poll, release, now_us, i2s_read, i2s_write};
static void setup(unsigned bits, unsigned channels)
{
    /* A separate, independently reset model per case. */
    memset(&capture, 0, sizeof(capture));
    memset(&dma, 0, sizeof(dma));
    memset(ir, 0, sizeof(ir)); memset(dr, 0, sizeof(dr));
    memset(samples, 0xa5, sizeof(samples)); memset(descriptors, 0x5a, sizeof(descriptors));
    dr[0x34/4] = 1;
    dr[ix(8)] = 0x41001000;
    ir[20/4] = 0x120; /* Preserve platform reset FIFO threshold. */
    now = acquired = released = writes = dma_started = invalidated = 0;
    acquire_refused = release_refused = flush_stuck = 0;
    pending_acquire = retained_acquire = late_resource = pending_release = 0;
    failing_read = failing_write = ignored_write = 0;
    cache_ticks = mapping_ticks = 0;
    format = (struct a733_i2s_format){24576000, 16000, 0, bits, 32, channels, 0};
    mapping = (struct a733_dma_mapping){samples, 0x41000100, 64, A733_DMA_WRITE};
    segment = (struct a733_dma_segment){{A733_I2S0_RXFIFO, 3, bits/8, 8},
                                       {0x41000100, 0, bits/8, 8}, 64};
    transfer = (struct a733_dma_transfer){{descriptors, 0x41000000, 64, A733_DMA_DESCRIPTORS},
                                         &segment, 1, &mapping, 1};
    struct a733_dma_operations ops = {dma_read, dma_write, cache, barrier, ticks,
                                      validate_mapping, validate_fifo};
    assert(a733_dma_controller_init(&dma, &ops, &dma, DMA_BASE, 1, 10) == A733_DMA_CONTROL_OK);
}
static void open_capture(void)
{
    assert(a733_i2s_open(&capture, &format, &iops, &capture, &dma, 0, 10000) == A733_I2S_READY);
    assert(ir[20/4] == 0x121);
}
static void start_capture(void)
{
    assert(a733_i2s_start(&capture, &transfer, 100) == A733_I2S_PREPARING);
    assert(a733_i2s_owns(&capture) && !dma_started);
    assert(a733_i2s_poll(&capture) == A733_I2S_ACTIVE && dma_started == 1);
}
static void close_capture(void)
{
    assert(!a733_i2s_owns(&capture));
    assert(a733_i2s_close(&capture, 10000) == A733_I2S_CLOSED);
    assert(released == 1 && !acquired && !dma.channels[0].claimed);
}
static void held_recovery(void)
{
    assert(a733_i2s_owns(&capture) && acquired && capture.dma_held);
    unsigned old_writes = writes;
    assert(a733_i2s_poll(&capture) == A733_I2S_RECOVERY);
    assert(a733_i2s_cancel(&capture) == A733_I2S_RECOVERY);
    assert(a733_i2s_close(&capture, 10000) == A733_I2S_RECOVERY);
    assert(writes == old_writes && !released && a733_i2s_owns(&capture));
}
int main(void)
{
    /* Finite completion: stop producer first, retain until delayed DMA drain,
     * and preserve raw captured bits, including 24-bit padding/sign patterns. */
    for (unsigned bits = 16; bits <= 32; bits += 16) {
        setup(bits, 2); open_capture(); start_capture();
        for (unsigned i = 0; i < 64; ++i) device_samples[i] = (unsigned char)(i * 37u + 0x80);
        dr[ix(0x38)] = 4;
        assert(a733_i2s_poll(&capture) == A733_I2S_STOPPING);
        assert(a733_i2s_owns(&capture) && !invalidated && samples[0] == 0xa5);
        assert(!(ir[0] & 2) && !ir[28/4]);
        dr[0x30/4] = 0;
        assert(a733_i2s_poll(&capture) == A733_I2S_COMPLETE && invalidated == 1);
        assert(!memcmp(samples, device_samples, 64));
        assert(a733_i2s_start(&capture, &transfer, 100) == A733_I2S_INVALID);
        close_capture();
    }
    setup(32,1); pending_acquire=1;
    assert(a733_i2s_open(&capture,&format,&iops,&capture,&dma,0,10000) == A733_I2S_OPENING);
    assert(!writes && capture.platform_held && !capture.dma_held);
    assert(a733_i2s_poll(&capture) == A733_I2S_OPENING && !writes);
    assert(a733_i2s_poll(&capture) == A733_I2S_READY);
    pending_release=1;
    assert(a733_i2s_close(&capture,10000) == A733_I2S_CLOSING && capture.platform_held);
    assert(a733_i2s_poll(&capture) == A733_I2S_CLOSED && !capture.platform_held);

    for (unsigned late=0;late<2;++late) {
        setup(32,1); retained_acquire=!late; late_resource=late;
        assert(a733_i2s_open(&capture,&format,&iops,&capture,&dma,0,10000) == A733_I2S_RECOVERY);
        assert(capture.platform_held && !capture.dma_held && !writes);
        assert(a733_i2s_poll(&capture) == A733_I2S_RECOVERY && !writes);
        assert(a733_i2s_close(&capture,20000) == A733_I2S_RECOVERY && !released);
    }
    setup(32,1); pending_acquire=1;
    assert(a733_i2s_open(&capture,&format,&iops,&capture,&dma,0,10000) == A733_I2S_OPENING);
    now=10000;
    assert(a733_i2s_poll(&capture) == A733_I2S_RECOVERY && capture.platform_held && !writes);
    setup(32,1); open_capture(); late_resource=1;
    assert(a733_i2s_close(&capture,10000) == A733_I2S_RECOVERY && capture.platform_held);

    setup(32, 1); acquire_refused = 1;
    assert(a733_i2s_open(&capture, &format, &iops, &capture, &dma, 0, 10000) == A733_I2S_CONFLICT);
    assert(!writes && !acquired && !dma.channels[0].claimed);
    setup(32, 1); assert(a733_dma_claim(&dma, 0) == A733_DMA_CONTROL_OK);
    assert(a733_i2s_open(&capture, &format, &iops, &capture, &dma, 0, 10000) == A733_I2S_CONFLICT);
    assert(!writes && !acquired && dma.channels[0].claimed);
    setup(32, 1); open_capture(); segment.source.request = 4;
    unsigned old_writes = writes;
    assert(a733_i2s_start(&capture, &transfer, 100) == A733_I2S_INVALID);
    assert(writes == old_writes && !a733_i2s_owns(&capture)); close_capture();
    setup(32, 1); open_capture(); mapping.cpu = &capture;
    assert(a733_i2s_start(&capture, &transfer, 100) == A733_I2S_INVALID); close_capture();
    setup(32, 1); open_capture(); mapping.bus += 64;
    assert(a733_i2s_start(&capture, &transfer, 100) == A733_I2S_PREPARING);
    assert(a733_i2s_poll(&capture) == A733_I2S_FAILED && !dma_started); close_capture();

    setup(32, 1); open_capture(); flush_stuck = 1;
    assert(a733_i2s_start(&capture, &transfer, 100) == A733_I2S_PREPARING);
    assert(a733_i2s_poll(&capture) == A733_I2S_PREPARING);
    now = 100;
    assert(a733_i2s_poll(&capture) == A733_I2S_FAILED && !dma_started); close_capture();
    setup(32, 1); open_capture();
    assert(a733_i2s_start(&capture, &transfer, 100) == A733_I2S_PREPARING);
    assert(a733_i2s_cancel(&capture) == A733_I2S_CANCELED && !dma_started); close_capture();

    /* Cancellation, FIFO overrun, DMA fault, and deadline all stop production
     * before draining; neither elapsed time nor a stop request releases RAM. */
    for (unsigned fault = 0; fault < 4; ++fault) {
        setup(32, 1); open_capture(); start_capture();
        if (fault == 0) assert(a733_i2s_cancel(&capture) == A733_I2S_STOPPING);
        if (fault == 1) ir[12/4] = 2;
        if (fault == 2) dr[ix(0x38)] = 8;
        if (fault == 3) now = 100;
        assert(a733_i2s_poll(&capture) == A733_I2S_STOPPING && a733_i2s_owns(&capture));
        dr[0x30/4] = 0;
        assert(a733_i2s_poll(&capture) == (fault ? A733_I2S_FAILED : A733_I2S_CANCELED));
        close_capture();
    }
    setup(32, 1); open_capture(); start_capture();
    assert(a733_i2s_cancel(&capture) == A733_I2S_STOPPING); now = 10;
    assert(a733_i2s_poll(&capture) == A733_I2S_RECOVERY); held_recovery();
    setup(32, 1); open_capture(); start_capture(); failing_write = A733_I2S0_BASE + 28;
    assert(a733_i2s_cancel(&capture) == A733_I2S_RECOVERY); held_recovery();
    setup(32, 1); open_capture(); start_capture(); failing_read = A733_I2S0_BASE + 12;
    assert(a733_i2s_poll(&capture) == A733_I2S_RECOVERY); held_recovery();
    setup(32, 1); open_capture(); start_capture(); dr[0x34/4] = 0; dr[ix(0x38)] = 4;
    assert(a733_i2s_poll(&capture) == A733_I2S_RECOVERY); held_recovery();
    setup(32, 1); open_capture(); start_capture(); ignored_write = A733_I2S0_BASE + 28;
    assert(a733_i2s_cancel(&capture) == A733_I2S_RECOVERY); held_recovery();
    setup(32, 1); open_capture();
    assert(a733_i2s_start(&capture, &transfer, 100) == A733_I2S_PREPARING);
    failing_write = A733_I2S0_BASE + 28;
    assert(a733_i2s_poll(&capture) == A733_I2S_RECOVERY && dma_started); held_recovery();
    setup(32, 1); open_capture();
    assert(a733_i2s_start(&capture, &transfer, 100) == A733_I2S_PREPARING);
    cache_ticks = 20; mapping_ticks = 40;
    assert(a733_i2s_poll(&capture) == A733_I2S_STOPPING && dma_started == 1);
    dr[0x30/4] = 0;
    assert(a733_i2s_poll(&capture) == A733_I2S_FAILED); close_capture();
    setup(32, 1); open_capture(); release_refused = 1;
    assert(a733_i2s_close(&capture, 10000) == A733_I2S_RECOVERY);
    assert(acquired && !capture.dma_held && !released);
    puts("I2S/native DMA capture: completion, ordering, isolation, deadlines, and retained faults passed");
    return 0;
}
