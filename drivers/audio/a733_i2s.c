#include "a733_i2s.h"

/* Register facts: pinned A733 audio sources and board route in docs/i2s.md. */
enum { CTL = 0, FMT0 = 4, FMT1 = 8, INTSTA = 12, FIFOCTL = 20,
       INTCTL = 28, CLKDIV = 36, RXCNT = 44, CHCFG = 48, RXCHSEL = 100,
       RXMAP0 = 104, RXMAP1 = 108, RXMAP2 = 112, RXMAP3 = 116 };
enum { GEN = 1, RXEN = 2, RXDRQ = 8, RX_ERRORS = 6, FLUSH = 1u << 24 };

static unsigned divider(uint32_t input, uint32_t output)
{
    static const unsigned divisors[] = {1,2,4,6,8,12,16,24,32,48,64,96,128,176,192};
    if (!output || input % output) return 0;
    for (unsigned i = 0; i < sizeof(divisors) / sizeof(*divisors); ++i)
        if (input / output == divisors[i]) return i + 1;
    return 0;
}

int a733_i2s_plan(const struct a733_i2s_format *f, struct a733_i2s_plan *out)
{
    if (!f || !out || f->rate_hz < 8000 || f->rate_hz > 192000 ||
        (f->container_bits != 16 && f->container_bits != 32) ||
        (f->slot_bits != 16 && f->slot_bits != 32) || f->slot_bits < f->container_bits ||
        (f->channels != 1 && f->channels != 2) || f->first_slot > 1 ||
        (f->channels == 2 && f->first_slot)) return 0;
    unsigned bclk = divider(f->module_hz, f->rate_hz * 2u * f->slot_bits);
    unsigned mclk = f->mclk_hz ? divider(f->module_hz, f->mclk_hz) : 0;
    if (!bclk || (f->mclk_hz && !mclk)) return 0;
    struct a733_i2s_plan p = {0};
    p.format = *f;
    p.ctl = (1u << 4) | (1u << 17) | (1u << 18);
    p.fmt0 = ((f->slot_bits - 1u) << 8) |
             ((f->container_bits / 4u - 1u) << 4) | (f->slot_bits / 4u - 1u);
    p.clkdiv = bclk << 4;
    if (mclk) p.clkdiv |= (1u << 8) | mclk;
    p.chcfg = (f->channels - 1u) << 4;
    p.rxchsel = (1u << 20) | ((f->channels - 1u) << 16);
    p.rxmap3 = f->channels == 2 ? 0x100u : f->first_slot;
    p.pb_mask = (1u << 5) | (1u << 6) | (1u << 8) | (mclk ? 1u << 4 : 0);
    p.dma_width = f->container_bits / 8;
    p.frame_bytes = p.dma_width * f->channels;
    *out = p;
    return 1;
}

static int read_reg(struct a733_i2s_capture *c, unsigned r, uint32_t *v)
{ return c->operations.read(c->cookie, A733_I2S0_BASE + r, v); }
static int write_reg(struct a733_i2s_capture *c, unsigned r, uint32_t v)
{ return c->operations.write(c->cookie, A733_I2S0_BASE + r, v); }
static int checked_write(struct a733_i2s_capture *c, unsigned r, uint32_t v)
{
    uint32_t got;
    return write_reg(c, r, v) && read_reg(c, r, &got) && got == v;
}
static uint64_t ticks(struct a733_i2s_capture *c)
{ return c->dma->operations.ticks(c->dma->cookie); }
static enum a733_i2s_status recovery(struct a733_i2s_capture *c)
{ return c->status = A733_I2S_RECOVERY; }

/* These writes precede every DMA stop/poll that can release buffers. */
static int stop_requests(struct a733_i2s_capture *c)
{
    return checked_write(c, INTCTL, 0) &&
           checked_write(c, CTL, c->plan.ctl | GEN);
}
static enum a733_i2s_status finish(struct a733_i2s_capture *c)
{
    if (c->transfer && a733_dma_owns(c->dma, c->transfer)) return recovery(c);
    if (!checked_write(c, CTL, c->plan.ctl)) return recovery(c);
    c->buffers_held = 0;
    c->transfer = NULL;
    return c->status = c->outcome;
}

enum a733_i2s_status a733_i2s_open(struct a733_i2s_capture *c,
    const struct a733_i2s_format *f, const struct a733_i2s_operations *ops, void *cookie,
    struct a733_dma_controller *dma, unsigned channel)
{
    struct a733_i2s_plan p;
    if (!c || c->status != A733_I2S_CLOSED || c->platform_held || c->dma_held ||
        !a733_i2s_plan(f, &p) || !ops || !ops->acquire || !ops->release ||
        !ops->read || !ops->write || !dma || !dma->initialized ||
        dma->base != 0x04601000u || channel >= A733_DMA_CHANNELS)
        return A733_I2S_INVALID;
    if (!ops->acquire(cookie, &p)) return A733_I2S_CONFLICT;
    c->plan = p;
    c->operations = *ops;
    c->cookie = cookie;
    c->dma = dma;
    c->channel = channel;
    c->platform_held = 1;
    if (a733_dma_claim(dma, channel) != A733_DMA_CONTROL_OK) {
        if (!ops->release(cookie)) return recovery(c);
        c->platform_held = 0;
        return A733_I2S_CONFLICT;
    }
    c->dma_held = 1;
    uint32_t ctl, interrupt, status, fifo;
    if (!read_reg(c, CTL, &ctl) || (ctl & 7u) ||
        !read_reg(c, INTCTL, &interrupt) || interrupt ||
        !read_reg(c, INTSTA, &status) || (status & RX_ERRORS) ||
        !read_reg(c, FIFOCTL, &fifo) || (fifo & ((1u << 31) | FLUSH | (1u << 25))))
        return recovery(c);
    if (!checked_write(c, CTL, p.ctl) || !checked_write(c, FMT0, p.fmt0) ||
        !checked_write(c, FMT1, 0) || !checked_write(c, CLKDIV, p.clkdiv) ||
        !checked_write(c, CHCFG, p.chcfg) || !checked_write(c, RXCHSEL, p.rxchsel) ||
        !checked_write(c, RXMAP0, 0) || !checked_write(c, RXMAP1, 0) ||
        !checked_write(c, RXMAP2, 0) || !checked_write(c, RXMAP3, p.rxmap3) ||
        !checked_write(c, FIFOCTL, (fifo & ~3u) | 1u)) return recovery(c);
    return c->status = A733_I2S_READY;
}

static int overlaps_context(struct a733_i2s_capture *c, const struct a733_dma_mapping *m)
{
    uintptr_t begin = (uintptr_t)m->cpu, ctx = (uintptr_t)c;
    return !m->cpu || m->bytes > UINTPTR_MAX - begin ||
           (begin < ctx + sizeof(*c) && ctx < begin + m->bytes);
}
static int valid_capture(struct a733_i2s_capture *c, const struct a733_dma_transfer *t)
{
    if (!t || !t->segments || !t->count || t->count > A733_DMA_MAX_SEGMENTS ||
        (uintptr_t)t->segments % _Alignof(struct a733_dma_segment) ||
        (uintptr_t)t->segments > UINTPTR_MAX - t->count * sizeof(*t->segments) ||
        !t->mappings || !t->mapping_count || t->mapping_count > A733_DMA_MAX_MAPPINGS ||
        (uintptr_t)t->mappings % _Alignof(struct a733_dma_mapping) ||
        (uintptr_t)t->mappings > UINTPTR_MAX - t->mapping_count * sizeof(*t->mappings) ||
        overlaps_context(c, &t->descriptors)) return 0;
    for (size_t i = 0; i < t->mapping_count; ++i)
        if (t->mappings[i].access != A733_DMA_WRITE || overlaps_context(c, &t->mappings[i])) return 0;
    for (size_t i = 0; i < t->count; ++i) {
        const struct a733_dma_segment *s = &t->segments[i];
        if (s->source.address != A733_I2S0_RXFIFO || s->source.request != A733_I2S0_DMA_REQUEST ||
            s->destination.request || s->source.width != c->plan.dma_width ||
            s->destination.width != c->plan.dma_width ||
            s->source.burst != 8 || s->destination.burst != 8 ||
            !s->bytes || s->bytes % c->plan.frame_bytes) return 0;
    }
    return 1;
}

enum a733_i2s_status a733_i2s_start(struct a733_i2s_capture *c,
    const struct a733_dma_transfer *t, uint64_t timeout)
{
    if (!c || c->status != A733_I2S_READY || !timeout || timeout > INT64_MAX ||
        !valid_capture(c, t)) return A733_I2S_INVALID;
    c->transfer = t;
    c->buffers_held = 1;
    c->started = ticks(c);
    c->timeout = timeout;
    c->outcome = A733_I2S_COMPLETE;
    uint32_t fifo;
    if (!checked_write(c, CTL, c->plan.ctl | GEN) || !read_reg(c, FIFOCTL, &fifo) ||
        !write_reg(c, FIFOCTL, fifo | FLUSH) || !checked_write(c, RXCNT, 0)) return recovery(c);
    return c->status = A733_I2S_PREPARING;
}

static enum a733_i2s_status drain(struct a733_i2s_capture *c)
{
    enum a733_dma_control_status s = a733_dma_poll(c->dma, c->channel);
    if (s == A733_DMA_STOPPING) return c->status;
    if (s != A733_DMA_COMPLETE && s != A733_DMA_CANCELED && s != A733_DMA_FAILED)
        return recovery(c);
    if (s == A733_DMA_FAILED) c->outcome = A733_I2S_FAILED;
    return finish(c);
}

enum a733_i2s_status a733_i2s_poll(struct a733_i2s_capture *c)
{
    if (!c) return A733_I2S_INVALID;
    if (c->status == A733_I2S_STOPPING) return drain(c);
    if (c->status != A733_I2S_PREPARING && c->status != A733_I2S_ACTIVE) return c->status;
    uint32_t status;
    if (!read_reg(c, INTSTA, &status)) return recovery(c);
    uint64_t elapsed = ticks(c) - c->started;
    if (c->status == A733_I2S_PREPARING) {
        uint32_t fifo;
        if (!read_reg(c, FIFOCTL, &fifo)) return recovery(c);
        if ((status & RX_ERRORS) || elapsed >= c->timeout) {
            c->outcome = A733_I2S_FAILED;
            if (!stop_requests(c)) return recovery(c);
            return finish(c);
        }
        if (fifo & FLUSH) return c->status;
        enum a733_dma_control_status s = a733_dma_submit(c->dma, c->channel,
                                                        c->transfer, c->timeout - elapsed);
        if (s == A733_DMA_ACTIVE) {
            if (ticks(c) - c->started >= c->timeout) {
                c->outcome = A733_I2S_FAILED;
                if (!stop_requests(c) ||
                    a733_dma_cancel(c->dma, c->channel) != A733_DMA_STOPPING)
                    return recovery(c);
                return c->status = A733_I2S_STOPPING;
            }
            if (!checked_write(c, CTL, c->plan.ctl | GEN | RXEN) ||
                !checked_write(c, INTCTL, RXDRQ)) return recovery(c);
            return c->status = A733_I2S_ACTIVE;
        }
        c->outcome = A733_I2S_FAILED;
        if (s == A733_DMA_RECOVERY) return recovery(c);
        if (!stop_requests(c)) return recovery(c);
        if (s == A733_DMA_STOPPING) {
            c->status = A733_I2S_STOPPING;
            return c->status;
        }
        return finish(c);
    }
    /* Read-only event peek: poll() in the DMA controller can stop the channel.
     * Disable the producer first, even on completion and deadline expiry.
     * Half/package events (bits 0/1) do not terminate the finite chain. */
    uint32_t pending = c->dma->operations.read(c->dma->cookie,
                           c->dma->base + 0x100u + c->channel * 0x40u + 0x38u);
    if (!(status & RX_ERRORS) && !(pending & ~3u) && elapsed < c->timeout) return c->status;
    if (!stop_requests(c)) return recovery(c);
    c->status = A733_I2S_STOPPING;
    if ((status & RX_ERRORS) || elapsed >= c->timeout) {
        c->outcome = A733_I2S_FAILED;
        if (a733_dma_cancel(c->dma, c->channel) != A733_DMA_STOPPING) return recovery(c);
    }
    return drain(c);
}

enum a733_i2s_status a733_i2s_cancel(struct a733_i2s_capture *c)
{
    if (!c) return A733_I2S_INVALID;
    if (c->status != A733_I2S_PREPARING && c->status != A733_I2S_ACTIVE) return c->status;
    if (!stop_requests(c)) return recovery(c);
    c->outcome = A733_I2S_CANCELED;
    if (c->status == A733_I2S_PREPARING) return finish(c);
    if (a733_dma_cancel(c->dma, c->channel) != A733_DMA_STOPPING) return recovery(c);
    return c->status = A733_I2S_STOPPING;
}

enum a733_i2s_status a733_i2s_close(struct a733_i2s_capture *c)
{
    if (!c) return A733_I2S_INVALID;
    if (c->status != A733_I2S_READY && c->status != A733_I2S_COMPLETE &&
        c->status != A733_I2S_CANCELED && c->status != A733_I2S_FAILED) return c->status;
    if (c->buffers_held || !checked_write(c, INTCTL, 0) ||
        !checked_write(c, CTL, c->plan.ctl)) return recovery(c);
    if (c->dma_held) {
        if (a733_dma_release(c->dma, c->channel) != A733_DMA_CONTROL_OK) return recovery(c);
        c->dma_held = 0;
    }
    if (!c->operations.release(c->cookie)) return recovery(c);
    c->platform_held = 0;
    return c->status = A733_I2S_CLOSED;
}

int a733_i2s_owns(const struct a733_i2s_capture *c)
{ return c && c->buffers_held; }
