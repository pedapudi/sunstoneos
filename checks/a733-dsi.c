#include "a733_dsi.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

void reference_initial(uint32_t *, unsigned);
void reference_packet(const struct a733_dsi_packet *, unsigned, unsigned, const void *, size_t);
void reference_registers(const uint32_t *, const uint32_t *, const struct a733_dsi_packet *);

struct model {
    uint32_t regs[256], initial[256];
    uint64_t now;
    unsigned calls, clocks, fail_at, late_at, corrupt_at, busy, hang, authority;
    unsigned controller, lanes, max_ops, reenter_at;
    struct a733_dsi *driver;
};

static int finish(struct model *m)
{
    ++m->calls;
    ++m->now;
    if (m->calls == m->late_at) m->now = 10000;
    if (m->calls == m->reenter_at)
        assert(a733_dsi_poll(m->driver) == A733_DSI_RETAINED);
    return m->calls != m->fail_at;
}
static int admit(void *p, unsigned controller, unsigned lanes, uint64_t lease)
{
    struct model *m = p;
    assert(controller == m->controller && lanes == m->lanes && lease == 91);
    return finish(m) && m->authority;
}
static unsigned index_for(struct model *m, uintptr_t address)
{
    uintptr_t base = 0x05506000u + m->controller * 0x2000u;
    assert(address >= base && address < base + 0x400 && !(address & 3));
    return (unsigned)(address - base) / 4;
}
static int read32(void *p, uintptr_t address, uint32_t *value)
{
    struct model *m = p;
    unsigned index = index_for(m, address);
    *value = m->regs[index];
    if (index == 4 && m->busy && !m->hang) {
        if (!--m->busy) m->regs[4] &= ~1u;
    }
    if (m->calls + 1 == m->corrupt_at) *value ^= 2;
    return finish(m);
}
static int write32(void *p, uintptr_t address, uint32_t value)
{
    struct model *m = p;
    unsigned index = index_for(m, address);
    assert(index == 4 || index == 0x48 / 4 || index == 0x200 / 4 || index >= 0x300 / 4);
    m->regs[index] = value; /* Failed writes may already have taken effect. */
    if (index == 4 && (value & 1)) m->busy = 2;
    return finish(m);
}
static int barrier(void *p) { return finish(p); }
static int now_us(void *p, uint64_t *value)
{
    struct model *m = p;
    ++m->clocks;
    *value = m->now;
    return 1;
}
static void setup(struct a733_dsi *d, struct model *m, unsigned controller, unsigned lanes)
{
    memset(d, 0, sizeof(*d));
    memset(m, 0, sizeof(*m));
    m->now = 1;
    m->authority = 1;
    m->controller = controller;
    m->lanes = lanes;
    m->driver = d;
    reference_initial(m->regs, lanes);
    memcpy(m->initial, m->regs, sizeof(m->regs));
    struct a733_dsi_config c = {
        .ops = {admit, read32, write32, barrier, now_us},
        .context = m, .controller = controller, .lanes = lanes, .lease = 91
    };
    assert(a733_dsi_init(d, &c));
    memset(&c, 0xcc, sizeof(c)); /* Configuration is copied. */
}
static enum a733_dsi_state run(struct a733_dsi *d, struct model *m)
{
    for (unsigned i = 0; i != 150; ++i) {
        unsigned before = m->calls;
        enum a733_dsi_state s = a733_dsi_poll(d);
        assert(m->calls - before <= 1);
        if (s != A733_DSI_PENDING) return s;
    }
    assert(!"unbounded poll sequence");
    return A733_DSI_RETAINED;
}
static void quiet(struct a733_dsi *d, struct model *m)
{
    unsigned calls = m->calls, clocks = m->clocks;
    enum a733_dsi_state state = d->state;
    for (unsigned i = 0; i != 2; ++i) assert(a733_dsi_poll(d) == state);
    assert(m->calls == calls && m->clocks == clocks);
}
static unsigned success(unsigned type, unsigned channel, unsigned n, unsigned controller, unsigned lanes)
{
    struct a733_dsi d;
    struct model m;
    uint8_t payload[250];
    for (unsigned i = 0; i != sizeof(payload); ++i) payload[i] = (uint8_t)(i * 37 + n);
    setup(&d, &m, controller, lanes);
    assert(a733_dsi_submit(&d, type, channel, payload, n, 10000));
    reference_packet(&d.packet, type, channel, payload, n);
    memset(payload, 0xee, sizeof(payload));
    assert(!a733_dsi_submit(&d, type, channel, payload, n, 10000));
    assert(run(&d, &m) == A733_DSI_ENGINE_DONE);
    reference_registers(m.initial, m.regs, &d.packet);
    quiet(&d, &m);
    unsigned calls = m.calls;
    assert(a733_dsi_reap(&d));
    assert(a733_dsi_submit(&d, 0x05, 0, payload, 1, 10000));
    assert(run(&d, &m) == A733_DSI_ENGINE_DONE);
    return calls;
}
static void faults(void)
{
    struct a733_dsi d;
    struct model m;
    uint8_t bytes[7] = {0x11, 2, 3, 4, 5, 6, 7};
    unsigned count = success(0x39, 2, sizeof(bytes), 1, 4);
    for (unsigned kind = 0; kind != 3; ++kind) {
        for (unsigned op = 1; op <= count; ++op) {
            setup(&d, &m, 1, 4);
            if (kind == 0) m.fail_at = op;
            if (kind == 1) m.late_at = op;
            if (kind == 2) m.reenter_at = op;
            assert(a733_dsi_submit(&d, 0x39, 2, bytes, sizeof(bytes), 10000));
            assert(run(&d, &m) == A733_DSI_RETAINED);
            assert(d.config.lease == 91 && d.packet.size == sizeof(bytes) + 6);
            assert(m.calls == op);
            assert(!a733_dsi_reap(&d));
            quiet(&d, &m);
        }
    }
    const unsigned bad_offsets[] = {0, 0x10, 4, 0x20, 0x30, 0x40, 0x44, 0x54, 0x4c, 0x50};
    const uint32_t bad_values[] = {0, 0x30001, 1, 0, 0, 0, 0, 0, 0x10000000, 0x10040000};
    for (unsigned i = 0; i != sizeof(bad_offsets)/sizeof(*bad_offsets); ++i) {
        setup(&d, &m, 0, 1);
        m.regs[bad_offsets[i] / 4] = bad_values[i];
        assert(a733_dsi_submit(&d, 0x05, 0, bytes, 1, 10000));
        assert(run(&d, &m) == A733_DSI_RETAINED);
        assert(d.error == A733_DSI_INITIAL_STATE && !d.writes_attempted);
    }
    /* The short packet has one SRAM write: verify size, jump and final control. */
    const unsigned corrupt[] = {14, 16, 18, 22};
    for (unsigned i = 0; i != sizeof(corrupt)/sizeof(*corrupt); ++i) {
        setup(&d, &m, 0, 4);
        m.corrupt_at = corrupt[i];
        assert(a733_dsi_submit(&d, 0x05, 0, bytes, 1, 10000));
        assert(run(&d, &m) == A733_DSI_RETAINED);
        assert(d.error == A733_DSI_VERIFY);
        quiet(&d, &m);
    }
    setup(&d, &m, 0, 4);
    m.authority = 0;
    assert(a733_dsi_submit(&d, 0x05, 0, bytes, 1, 10000));
    assert(run(&d, &m) == A733_DSI_RETAINED && !d.writes_attempted);
    assert(d.error == A733_DSI_AUTHORITY);
    setup(&d, &m, 0, 4);
    m.hang = 1;
    assert(a733_dsi_submit(&d, 0x05, 0, bytes, 1, 40));
    assert(run(&d, &m) == A733_DSI_RETAINED && (m.regs[4] & 1));
    assert(d.error == A733_DSI_TIME);
    quiet(&d, &m);
    setup(&d, &m, 0, 4);
    assert(a733_dsi_submit(&d, 0x05, 0, bytes, 1, 10000));
    assert(a733_dsi_poll(&d) == A733_DSI_PENDING);
    m.now = 0;
    assert(a733_dsi_poll(&d) == A733_DSI_RETAINED && m.calls == 1);
    quiet(&d, &m);
    setup(&d, &m, 0, 4);
    assert(a733_dsi_submit(&d, 0x05, 0, bytes, 1, 1));
    assert(a733_dsi_poll(&d) == A733_DSI_RETAINED && m.calls == 0);
    printf("PASS retained failure, deadline and reentry at each of %u operations\n", count);
}
static void refusal(void)
{
    struct a733_dsi_packet out, before;
    memset(&out, 0xa5, sizeof(out)); before = out;
    uint8_t data[251] = {0};
    assert(!a733_dsi_encode(&out, 0x39, 0, data, 251));
    assert(!a733_dsi_encode(&out, 0x39, 0, data, 0));
    assert(!a733_dsi_encode(&out, 0x39, 4, data, 1));
    assert(!a733_dsi_encode(&out, 0x06, 0, data, 1));
    assert(!a733_dsi_encode(&out, 0x05, 0, NULL, 1));
    assert(!a733_dsi_encode(&out, 0x03, 0, data, 1));
    assert(!a733_dsi_encode(&out, 0x15, 0, data, 1));
    assert(!a733_dsi_encode(&out, 0x39, 0, out.bytes, 1));
    assert(!a733_dsi_encode(&out, 0x39, 0, (void *)(UINTPTR_MAX - 1), 3));
    assert(!memcmp(&out, &before, sizeof(out)));
    struct a733_dsi d, saved;
    struct model m;
    setup(&d, &m, 0, 4);
    saved = d;
    assert(!a733_dsi_init(&d, &d.config));
    assert(!a733_dsi_submit(&d, 0x39, 0, d.packet.bytes, 1, 10000));
    assert(!a733_dsi_submit(&d, 0x05, 0, data, 1, 0));
    assert(!a733_dsi_submit(&d, 0x39, 0, data, 251, 10000));
    assert(!memcmp(&d, &saved, sizeof(d)) && !m.calls && !m.clocks);
}
int main(void)
{
    const unsigned types[] = {0x03, 0x13, 0x23, 0x05, 0x15};
    const unsigned sizes[] = {0, 1, 2, 1, 2};
    const unsigned longs[] = {1, 2, 3, 4, 5, 16, 249, 250};
    unsigned comparisons = 0;
    for (unsigned vc = 0; vc != 4; ++vc) {
        for (unsigned i = 0; i != sizeof(types)/sizeof(*types); ++i) {
            success(types[i], vc, sizes[i], vc & 1, vc + 1); ++comparisons;
        }
        for (unsigned i = 0; i != sizeof(longs)/sizeof(*longs); ++i) {
            success(0x29, vc, longs[i], vc & 1, vc + 1);
            success(0x39, vc, longs[i], vc & 1, vc + 1);
            comparisons += 2;
        }
    }
    refusal();
    faults();
    printf("PASS %u complete packet/register comparisons, bounds and atomic refusal\n", comparisons);
    return 0;
}
