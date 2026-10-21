#include "a733_dma_controller.h"
#include "a733_dma_regs.h"
#include <string.h>

enum { ENABLE = A733_DMA_REG_ENABLE, PAUSE = A733_DMA_REG_PAUSE, DESCRIPTOR = A733_DMA_REG_DESCRIPTOR,
       DESTINATION = A733_DMA_REG_DESTINATION, REMAINING = A733_DMA_REG_REMAINING,
       MODE = A733_DMA_REG_MODE, PACKAGES = A733_DMA_REG_PACKAGES, IRQ_ENABLE = A733_DMA_REG_IRQ_ENABLE,
       PENDING = A733_DMA_REG_PENDING, DESCRIPTOR_HIGH = A733_DMA_REG_DESCRIPTOR_HIGH };
enum { BUSY = A733_DMA_REG_BUSY, CPU_ROUTE = A733_DMA_REG_CPU_ROUTE, MCU_ROUTE = A733_DMA_REG_MCU_ROUTE };

static uint32_t read_global(struct a733_dma_controller *c, unsigned offset)
{ return c->operations.read(c->cookie, c->base + offset); }
static uint32_t read_channel(struct a733_dma_controller *c, unsigned channel, unsigned offset)
{ return read_global(c, A733_DMA_REG_CHANNELS + channel * A733_DMA_REG_CHANNEL_STRIDE + offset); }
static void write_channel(struct a733_dma_controller *c, unsigned channel,
                          unsigned offset, uint32_t value)
{ c->operations.write(c->cookie, c->base + A733_DMA_REG_CHANNELS + channel * A733_DMA_REG_CHANNEL_STRIDE + offset, value); }
static void barrier(struct a733_dma_controller *c)
{ c->operations.barrier(c->cookie); }
static uint64_t ticks(struct a733_dma_controller *c)
{ return c->operations.ticks(c->cookie); }
static int valid_channel(const struct a733_dma_controller *c, unsigned channel)
{ return c && c->initialized && channel < A733_DMA_CHANNELS; }

/* Every range passed here was checked for overflow. */
static int cpu_overlap(const struct a733_dma_mapping *a, const void *b, size_t n)
{ return a733_dma_overlap((uintptr_t)a->cpu, a->bytes, (uintptr_t)b, n); }
static int metadata_overlap(const struct a733_dma_mapping *m, const struct a733_dma_transfer *t)
{
    return cpu_overlap(m, t, sizeof(*t)) ||
           cpu_overlap(m, t->segments, t->count * sizeof(*t->segments)) ||
           cpu_overlap(m, t->mappings, t->mapping_count * sizeof(*t->mappings));
}

static int valid_mapping(struct a733_dma_controller *c, const struct a733_dma_mapping *m)
{
    if (!a733_dma_mapping_shape(m)) return 0;
    if (m->access != A733_DMA_READ && m->access != A733_DMA_WRITE &&
        m->access != A733_DMA_DESCRIPTORS) return 0;
    if (cpu_overlap(m, c, sizeof(*c))) return 0;
    return c->operations.validate_mapping(c->cookie, m);
}

static int endpoint_mapped(const struct a733_dma_transfer *t,
                           const struct a733_dma_endpoint *e, uint32_t bytes,
                           enum a733_dma_access access)
{
    if (e->request) return 1;
    for (size_t i = 0; i < t->mapping_count; ++i) {
        const struct a733_dma_mapping *m = &t->mappings[i];
        if (m->access == access && e->address >= m->bus &&
            e->address - m->bus < m->bytes && bytes <= m->bytes - (e->address - m->bus))
            return 1;
    }
    return 0;
}

static int fifo_memory_conflict(const struct a733_dma_endpoint *e,
                                const struct a733_dma_transfer *t)
{
    for (size_t j = 0; j < t->mapping_count; ++j)
        if (a733_dma_overlap(e->address, e->width, t->mappings[j].bus, t->mappings[j].bytes)) return 1;
    return a733_dma_overlap(e->address, e->width, t->descriptors.bus, t->descriptors.bytes);
}

static int fifo_conflict(const struct a733_dma_transfer *a, const struct a733_dma_transfer *b)
{
    for (size_t i = 0; i < a->count; ++i) {
        const struct a733_dma_endpoint *ends[] = {&a->segments[i].source, &a->segments[i].destination};
        for (unsigned e = 0; e < 2; ++e) {
            if (!ends[e]->request) continue;
            for (size_t j = 0; j < b->count; ++j) {
                const struct a733_dma_endpoint *other[] = {&b->segments[j].source, &b->segments[j].destination};
                for (unsigned o = 0; o < 2; ++o)
                    /* Read and write FIFOs may share one MMIO address (UART RBR/
                     * THR). Each direction still needs independent native port
                     * admission; two readers or two writers remain exclusive. */
                    if (e == o && other[o]->request && a733_dma_overlap(ends[e]->address, ends[e]->width,
                                                     other[o]->address, other[o]->width)) return 1;
            }
            if (fifo_memory_conflict(ends[e], b)) return 1;
        }
    }
    return 0;
}

static enum a733_dma_control_status validate_transfer(
    struct a733_dma_controller *c, const struct a733_dma_transfer *t)
{
    if (!t || !t->segments || !t->mappings || !t->count ||
        t->count > A733_DMA_MAX_SEGMENTS || !t->mapping_count ||
        t->mapping_count > A733_DMA_MAX_MAPPINGS ||
        (uintptr_t)t > UINTPTR_MAX - sizeof(*t) ||
        (uintptr_t)t->segments % _Alignof(struct a733_dma_segment) ||
        (uintptr_t)t->mappings % _Alignof(struct a733_dma_mapping) ||
        (uintptr_t)t->segments > UINTPTR_MAX - t->count * sizeof(*t->segments) ||
        (uintptr_t)t->mappings > UINTPTR_MAX - t->mapping_count * sizeof(*t->mappings))
        return A733_DMA_INVALID;
    if (!valid_mapping(c, &t->descriptors) || t->descriptors.access != A733_DMA_DESCRIPTORS ||
        t->descriptors.bus >= UINT64_C(0xfffff800) ||
        t->descriptors.bytes > UINT64_C(0xfffff800) - t->descriptors.bus ||
        t->descriptors.bytes / 64 < t->count || metadata_overlap(&t->descriptors, t))
        return A733_DMA_INVALID;
    for (size_t i = 0; i < t->mapping_count; ++i) {
        const struct a733_dma_mapping *m = &t->mappings[i];
        if (!valid_mapping(c, m) || m->access == A733_DMA_DESCRIPTORS) return A733_DMA_INVALID;
        if (metadata_overlap(m, t) || a733_dma_mappings_overlap(m, &t->descriptors)) return A733_DMA_CONFLICT;
        for (size_t j = 0; j < i; ++j)
            if (a733_dma_mappings_overlap(m, &t->mappings[j])) return A733_DMA_CONFLICT;
    }
    for (size_t i = 0; i < t->count; ++i) {
        const struct a733_dma_segment *s = &t->segments[i];
        const struct a733_dma_endpoint *ends[] = {&s->source, &s->destination};
        for (unsigned e = 0; e < 2; ++e) {
            if (!ends[e]->request) continue;
            if (ends[e]->request > 63 ||
                (ends[e]->width != 1 && ends[e]->width != 2 && ends[e]->width != 4) ||
                ends[e]->address >= (UINT64_C(1) << 35) ||
                ends[e]->width > (UINT64_C(1) << 35) - ends[e]->address ||
                !c->operations.validate_fifo(c->cookie, ends[e], e ? A733_DMA_WRITE : A733_DMA_READ))
                return A733_DMA_INVALID;
            if (fifo_memory_conflict(ends[e], t)) return A733_DMA_CONFLICT;
        }
        if (!endpoint_mapped(t, &s->source, s->bytes, A733_DMA_READ) ||
            !endpoint_mapped(t, &s->destination, s->bytes, A733_DMA_WRITE)) return A733_DMA_INVALID;
    }
    for (unsigned channel = 0; channel < A733_DMA_CHANNELS; ++channel) {
        const struct a733_dma_transfer *other = c->channels[channel].transfer;
        if (!other) continue;
        if (t == other || metadata_overlap(&t->descriptors, other) ||
            metadata_overlap(&other->descriptors, t) ||
            a733_dma_mappings_overlap(&t->descriptors, &other->descriptors)) return A733_DMA_CONFLICT;
        for (size_t i = 0; i < t->mapping_count; ++i) {
            const struct a733_dma_mapping *m = &t->mappings[i];
            if (metadata_overlap(m, other) || a733_dma_mappings_overlap(m, &other->descriptors)) return A733_DMA_CONFLICT;
            for (size_t j = 0; j < other->mapping_count; ++j)
                if ((m->access != A733_DMA_READ || other->mappings[j].access != A733_DMA_READ) &&
                    a733_dma_mappings_overlap(m, &other->mappings[j])) return A733_DMA_CONFLICT;
        }
        for (size_t j = 0; j < other->mapping_count; ++j)
            if (metadata_overlap(&other->mappings[j], t) ||
                a733_dma_mappings_overlap(&t->descriptors, &other->mappings[j])) return A733_DMA_CONFLICT;
        if (fifo_conflict(t, other) || fifo_conflict(other, t)) return A733_DMA_CONFLICT;
    }
    return A733_DMA_CONTROL_OK;
}

static int routed(struct a733_dma_controller *c, unsigned channel)
{
    uint32_t bit = 1u << channel;
    return (c->authorized_channels & bit) && (read_global(c, CPU_ROUTE) & bit) &&
           !(read_global(c, MCU_ROUTE) & bit);
}
static int idle(struct a733_dma_controller *c, unsigned channel)
{
    return !read_channel(c, channel, ENABLE) && !(read_global(c, BUSY) & (1u << channel));
}
static int quiet(struct a733_dma_controller *c, unsigned channel)
{
    return routed(c, channel) && idle(c, channel) &&
           !read_channel(c, channel, PAUSE) && !read_channel(c, channel, IRQ_ENABLE) &&
           !read_channel(c, channel, PENDING) && !read_channel(c, channel, MODE) &&
           !read_channel(c, channel, DESCRIPTOR_HIGH);
}
static enum a733_dma_control_status recovery(struct a733_dma_channel *channel)
{ return channel->status = A733_DMA_RECOVERY; }

enum a733_dma_control_status a733_dma_controller_init(
    struct a733_dma_controller *c, const struct a733_dma_operations *ops,
    void *cookie, uintptr_t base, uint16_t authorized_channels, uint64_t stop_timeout)
{
    if (!c || c->initialized || !ops || !ops->read || !ops->write || !ops->cache ||
        !ops->barrier || !ops->ticks || !ops->validate_mapping || !ops->validate_fifo || !authorized_channels ||
        (base & 0xfff) || base > UINTPTR_MAX - 0x2000 ||
        !stop_timeout || stop_timeout > INT64_MAX) return A733_DMA_INVALID;
    c->operations = *ops;
    c->cookie = cookie;
    c->base = base;
    c->authorized_channels = authorized_channels;
    c->stop_timeout = stop_timeout;
    c->initialized = 1;
    return A733_DMA_CONTROL_OK;
}

enum a733_dma_control_status a733_dma_claim(struct a733_dma_controller *c, unsigned channel)
{
    if (!valid_channel(c, channel)) return A733_DMA_INVALID;
    struct a733_dma_channel *s = &c->channels[channel];
    if (s->claimed) return A733_DMA_CONFLICT;
    if (!quiet(c, channel)) return A733_DMA_UNAVAILABLE;
    s->saved_descriptor = read_channel(c, channel, DESCRIPTOR);
    s->status = A733_DMA_CONTROL_OK;
    s->claimed = 1;
    return A733_DMA_CONTROL_OK;
}

enum a733_dma_control_status a733_dma_release(struct a733_dma_controller *c, unsigned channel)
{
    if (!valid_channel(c, channel) || !c->channels[channel].claimed) return A733_DMA_INVALID;
    struct a733_dma_channel *s = &c->channels[channel];
    if (s->status == A733_DMA_RECOVERY) return s->status;
    if (s->transfer) return A733_DMA_CONFLICT;
    if (!quiet(c, channel) || read_channel(c, channel, DESCRIPTOR) != s->saved_descriptor)
        return recovery(s);
    s->claimed = 0;
    s->status = A733_DMA_CONTROL_OK;
    return A733_DMA_CONTROL_OK;
}

static void begin_stop(struct a733_dma_controller *c, unsigned channel,
                        enum a733_dma_control_status outcome)
{
    struct a733_dma_channel *s = &c->channels[channel];
    s->outcome = outcome;
    s->status = A733_DMA_STOPPING;
    s->stop_started = ticks(c);
    write_channel(c, channel, IRQ_ENABLE, 0);
    write_channel(c, channel, PAUSE, 1);
    write_channel(c, channel, ENABLE, 0);
    write_channel(c, channel, PAUSE, 0);
    barrier(c);
}

static enum a733_dma_control_status submit(
    struct a733_dma_controller *c, unsigned channel,
    const struct a733_dma_transfer *t, uint64_t timeout, unsigned cyclic)
{
    if (!valid_channel(c, channel) || !c->channels[channel].claimed ||
        !timeout || timeout > INT64_MAX) return A733_DMA_INVALID;
    struct a733_dma_channel *s = &c->channels[channel];
    if (s->status == A733_DMA_RECOVERY) return s->status;
    if (s->transfer) return A733_DMA_CONFLICT;
    enum a733_dma_control_status status = validate_transfer(c, t);
    if (status != A733_DMA_CONTROL_OK) return status;
    if(cyclic) {
        for(size_t i=0;i<t->mapping_count;++i)if(t->mappings[i].access!=(cyclic==1?A733_DMA_READ:A733_DMA_WRITE))return A733_DMA_INVALID;
        if(t->mapping_count!=t->count)return A733_DMA_INVALID;
        for(size_t i=0;i<t->count;++i) {
            const struct a733_dma_endpoint *ram=cyclic==1?&t->segments[i].source:&t->segments[i].destination;
            const struct a733_dma_endpoint *fifo=cyclic==1?&t->segments[i].destination:&t->segments[i].source;
            if(ram->request || !fifo->request || ram->address!=t->mappings[i].bus ||
               t->segments[i].bytes!=t->mappings[i].bytes)return A733_DMA_INVALID;
        }
    }
    if (!quiet(c, channel) || read_channel(c, channel, DESCRIPTOR) != s->saved_descriptor)
        return recovery(s);
    enum a733_dma_result encoded=cyclic?
        a733_dma_build_cyclic(t->descriptors.cpu,t->descriptors.bytes/64,t->descriptors.bus,t->segments,t->count):
        a733_dma_build(t->descriptors.cpu,t->descriptors.bytes/64,t->descriptors.bus,t->segments,t->count);
    if(encoded!=A733_DMA_OK)return A733_DMA_INVALID;
    s->transfer = t;s->cyclic=cyclic;s->package_last=0;s->packages=0;
    s->timeout = timeout;
    s->started = ticks(c);
    s->status = A733_DMA_ACTIVE;
    for (size_t i = 0; i < t->mapping_count; ++i) {
        const struct a733_dma_mapping *m = &t->mappings[i];
        c->operations.cache(c->cookie, m->access == A733_DMA_READ ?
                            A733_DMA_CLEAN : A733_DMA_CLEAN_INVALIDATE, m->cpu, m->bytes);
    }
    c->operations.cache(c->cookie, A733_DMA_CLEAN, t->descriptors.cpu, t->descriptors.bytes);
    barrier(c);
    write_channel(c, channel, DESCRIPTOR, (uint32_t)t->descriptors.bus);
    uint32_t irq=cyclic?10:12; /* Cyclic package+timeout; finite queue+timeout. */
    write_channel(c, channel, IRQ_ENABLE, irq);
    barrier(c);
    if (read_channel(c, channel, DESCRIPTOR) != t->descriptors.bus ||
        read_channel(c, channel, IRQ_ENABLE) != irq ||
        ticks(c) - s->started >= s->timeout) {
        begin_stop(c, channel, A733_DMA_FAILED);
        return cyclic?recovery(s):s->status;
    }
    write_channel(c, channel, ENABLE, 1);
    barrier(c);
    s->observed=ticks(c);
    /* The concrete I2S caller has not enabled DRQ yet. Reject a nonzero
     * transmission counter rather than assuming an inherited reset value. */
    if(cyclic && read_channel(c,channel,PACKAGES)) {
        begin_stop(c,channel,A733_DMA_FAILED);
        return recovery(s); /* Counter origin is untrusted; never export it. */
    }
    return s->status;
}
enum a733_dma_control_status a733_dma_submit(struct a733_dma_controller *c,unsigned channel,
    const struct a733_dma_transfer *t,uint64_t timeout)
{return submit(c,channel,t,timeout,0);}
enum a733_dma_control_status a733_dma_submit_cyclic_tx(struct a733_dma_controller *c,unsigned channel,
    const struct a733_dma_transfer *t,uint64_t timeout)
{return submit(c,channel,t,timeout,1);}
enum a733_dma_control_status a733_dma_submit_cyclic_rx(struct a733_dma_controller *c,unsigned channel,
    const struct a733_dma_transfer *t,uint64_t timeout)
{return submit(c,channel,t,timeout,2);}
static int cyclic_progress(struct a733_dma_controller *c,unsigned channel)
{
    struct a733_dma_channel *s=&c->channels[channel];
    uint64_t time=ticks(c);uint32_t count=read_channel(c,channel,PACKAGES);
    uint32_t delta=count-s->package_last;
    if(time-s->observed>=s->timeout || delta>INT32_MAX || s->packages>UINT64_MAX-delta)return 0;
    s->observed=time;s->package_last=count;s->packages+=delta;
    if(delta)s->started=time;
    return 1;
}
int a733_dma_cyclic_progress(const struct a733_dma_controller *c,unsigned channel,uint64_t *out)
{
    if(!out || !valid_channel(c,channel) || !c->channels[channel].claimed ||
       !c->channels[channel].cyclic || c->channels[channel].status==A733_DMA_RECOVERY)return 0;
    *out=c->channels[channel].packages;return 1;
}

/* This phase consistency check is not a completion fence. The manual does
 * not specify PKG_NUM/write/cursor ordering; physical RX admission remains
 * unqualified. In particular BCNT_LEFT can precede destination arrival.
 * LLI_ADDR is the next descriptor: the pinned Linux residue routine matches
 * lli->p_lli_next to this register. A count/cursor transition is deferred. */
static int rx_position(struct a733_dma_controller *c,unsigned channel)
{
    struct a733_dma_channel *s=&c->channels[channel];const struct a733_dma_transfer *t=s->transfer;
    uint32_t before=read_channel(c,channel,PACKAGES);
    uint32_t link=read_channel(c,channel,DESCRIPTOR);
    uint32_t destination=read_channel(c,channel,DESTINATION);
    uint32_t remaining=read_channel(c,channel,REMAINING);
    uint32_t again=read_channel(c,channel,DESCRIPTOR);
    uint32_t after=read_channel(c,channel,PACKAGES);
    if(before!=after || link!=again || after!=s->package_last)return 0;
    unsigned current=(unsigned)(s->packages%t->count);
    const struct a733_dma_mapping *m=&t->mappings[current];
    uint64_t expected=t->descriptors.bus+((current+1)%t->count)*64;
    if(link!=expected || m->bus>UINT32_MAX || destination<m->bus ||
       destination-m->bus>m->bytes || remaining!=m->bytes-(destination-m->bus))return 0;
    return 1;
}
int a733_dma_cyclic_rx_copy(struct a733_dma_controller *c,unsigned channel,uint64_t period,
    void *out,size_t bytes)
{
    if(!out || !bytes || !valid_channel(c,channel))return -1;
    struct a733_dma_channel *s=&c->channels[channel];const struct a733_dma_transfer *t=s->transfer;
    if(s->cyclic!=2 || s->status!=A733_DMA_ACTIVE || !t ||
       period>UINT64_MAX-t->count || bytes!=t->segments[0].bytes ||
       (uintptr_t)out>UINTPTR_MAX-bytes || cpu_overlap(&t->descriptors,out,bytes))return -1;
    for(size_t i=0;i<t->mapping_count;++i)if(cpu_overlap(&t->mappings[i],out,bytes))return -1;
    if(cpu_overlap(&(struct a733_dma_mapping){c,0,sizeof(*c),A733_DMA_READ},out,bytes))return -1;
    if(!cyclic_progress(c,channel))return -1;
    if(s->packages<=period)return 0;
    if(s->packages>=period+t->count)return -1;
    if(!rx_position(c,channel))return 0;
    const struct a733_dma_mapping *m=&t->mappings[period%t->count];
    /* The barrier orders the copy after invalidation. The CPU only reads the
     * raw range and never dirties it; the next copy of this period invalidates
     * it again, so no maintenance follows the copy. */
    c->operations.cache(c->cookie,A733_DMA_INVALIDATE,m->cpu,m->bytes);barrier(c);
    memcpy(out,m->cpu,bytes);
    if(!cyclic_progress(c,channel) || s->packages>=period+t->count ||
       !rx_position(c,channel) || (read_channel(c,channel,PENDING)&~3u))return -1;
    return 1;
}

enum a733_dma_control_status a733_dma_finite_state(struct a733_dma_controller *c, unsigned channel,
                                                   const struct a733_dma_transfer *t)
{
    if (!valid_channel(c, channel) || !c->channels[channel].claimed) return A733_DMA_INVALID;
    struct a733_dma_channel *s = &c->channels[channel];
    if (s->status != A733_DMA_ACTIVE || s->cyclic || !t || s->transfer != t) return A733_DMA_INVALID;
    if (!routed(c, channel) || read_channel(c, channel, MODE) ||
        read_channel(c, channel, DESCRIPTOR_HIGH)) return recovery(s);
    /* Bits 0/1 (half/package end) do not terminate a finite chain. */
    uint32_t pending = read_channel(c, channel, PENDING);
    if (pending & ~15u) return recovery(s);
    if ((pending & 8) || ticks(c) - s->started >= s->timeout) return A733_DMA_FAILED;
    return (pending & 4) ? A733_DMA_COMPLETE : A733_DMA_ACTIVE;
}

enum a733_dma_control_status a733_dma_cancel(struct a733_dma_controller *c, unsigned channel)
{
    if (!valid_channel(c, channel) || !c->channels[channel].claimed) return A733_DMA_INVALID;
    struct a733_dma_channel *s = &c->channels[channel];
    if (s->status != A733_DMA_ACTIVE) return s->status;
    if (!routed(c, channel) || read_channel(c, channel, MODE) ||
        read_channel(c, channel, DESCRIPTOR_HIGH)) return recovery(s);
    begin_stop(c, channel, A733_DMA_CANCELED);
    return s->status;
}

enum a733_dma_control_status a733_dma_poll(struct a733_dma_controller *c, unsigned channel)
{
    if (!valid_channel(c, channel) || !c->channels[channel].claimed) return A733_DMA_INVALID;
    struct a733_dma_channel *s = &c->channels[channel];
    if (s->status != A733_DMA_ACTIVE && s->status != A733_DMA_STOPPING) return s->status;
    if (!routed(c, channel) || read_channel(c, channel, MODE) ||
        read_channel(c, channel, DESCRIPTOR_HIGH)) return recovery(s);
    uint32_t pending = read_channel(c, channel, PENDING);
    if (pending & ~15u) return recovery(s);
    if (pending) write_channel(c, channel, PENDING, pending);
    if (s->status == A733_DMA_ACTIVE) {
        int ambiguous=s->cyclic && !cyclic_progress(c,channel);
        if (ambiguous || (pending & 8)) begin_stop(c, channel, A733_DMA_FAILED);
        else if (pending & 4) begin_stop(c, channel,s->cyclic?A733_DMA_FAILED:A733_DMA_COMPLETE);
        else if (ticks(c) - s->started >= s->timeout) begin_stop(c, channel, A733_DMA_FAILED);
        else return s->status;
    } else if (pending & 8) s->outcome = A733_DMA_FAILED;

    if (idle(c, channel)) {
        if(s->cyclic && !cyclic_progress(c,channel))return recovery(s);
        /* A late event is handled by the next bounded poll. */
        barrier(c);
        if (!read_channel(c, channel, PENDING)) {
            write_channel(c, channel, DESCRIPTOR, s->saved_descriptor);
            barrier(c);
            if (!quiet(c, channel) || read_channel(c, channel, DESCRIPTOR) != s->saved_descriptor)
                return recovery(s);
            const struct a733_dma_transfer *t = s->transfer;
            for (size_t i = 0; i < t->mapping_count; ++i) {
                const struct a733_dma_mapping *m = &t->mappings[i];
                if (m->access == A733_DMA_WRITE)
                    c->operations.cache(c->cookie, A733_DMA_INVALIDATE, m->cpu, m->bytes);
            }
            barrier(c);
            s->transfer = NULL;
            s->status = s->outcome;
            return s->status;
        }
    }
    if (ticks(c) - s->stop_started >= c->stop_timeout) return recovery(s);
    return s->status;
}

int a733_dma_owns(const struct a733_dma_controller *c, const struct a733_dma_transfer *t)
{
    if (!c || !c->initialized || !t) return 0;
    for (unsigned i = 0; i < A733_DMA_CHANNELS; ++i)
        if (c->channels[i].transfer == t) return 1;
    return 0;
}
