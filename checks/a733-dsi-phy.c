#include "a733_dsi_phy.h"
#include "a733_dsi.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

void phy_reference_factors(uint32_t, uint32_t, uint32_t);
void phy_reference_compare(const uint32_t *, const uint32_t *, const uint32_t *, const uint32_t *, unsigned);

struct model {
    uint32_t phy[128], host[256], ccu[2], original_phy[128], original_host[256];
    uint64_t now, last_lock, lock_ready, analog_time, delay_time;
    unsigned controller, lanes, calls, clocks, fail_at, late_at, corrupt_at;
    unsigned lock_reads, unlock, no_lock, authority, writes, busy, delay_seen;
    struct a733_dsi_phy *driver;
};
static uint32_t *word(struct model *m, uintptr_t address)
{
    uintptr_t phy = 0x05507000u + m->controller * 0x2000u;
    uintptr_t host = phy - 0x1000;
    uintptr_t ccu = 0x02003580u + m->controller * 8u;
    assert(!(address & 3));
    if (address >= phy && address < phy + sizeof(m->phy)) return &m->phy[(address - phy) / 4];
    if (address >= host && address < host + sizeof(m->host)) return &m->host[(address - host) / 4];
    assert(address == ccu || address == ccu + 4);
    return &m->ccu[(address - ccu) / 4];
}
static int finish(struct model *m)
{
    ++m->calls;
    if (m->calls == m->late_at) m->now = 10000;
    return m->calls != m->fail_at;
}
static int admit(void *ctx, const struct a733_dsi_phy_config *c)
{
    struct model *m = ctx;
    assert(c->controller == m->controller && c->lanes == m->lanes && c->lease == 444);
    return finish(m) && m->authority;
}
static int read32(void *ctx, uintptr_t address, uint32_t *value)
{
    struct model *m = ctx;
    uint32_t *p = word(m, address);
    *value = *p;
    if (p == &m->phy[0xe0 / 4]) {
        if (m->last_lock) assert(m->now - m->last_lock >= 5);
        m->last_lock = m->now;
        ++m->lock_reads;
        if (m->no_lock || (m->unlock && m->lock_reads == 2)) *value &= ~(1u << 26);
        m->lock_ready = m->now;
    }
    if (p == &m->host[4] && m->busy && !--m->busy) m->host[4] &= ~1u;
    if (m->calls + 1 == m->corrupt_at) *value ^= 1;
    return finish(m);
}
static int write32(void *ctx, uintptr_t address, uint32_t value)
{
    struct model *m = ctx;
    uint32_t *p = word(m, address);
    /* This component cannot write a PLL, CCU gate or shared reset. */
    assert(p != &m->ccu[0] && p != &m->ccu[1]);
    assert((uintptr_t)p < (uintptr_t)&m->phy[0x104 / 4] ||
           (uintptr_t)p > (uintptr_t)&m->phy[0x10c / 4]);
    if (!m->writes) {
        assert(m->lock_reads >= 3 && m->now - m->lock_ready >= 20);
        m->analog_time = m->now;
    }
    if (p == &m->phy[0x118 / 4]) m->delay_time = m->now;
    if (p == &m->phy[0x58 / 4] && !(m->phy[0x58 / 4] & (1u << 27)) && (value & (1u << 27))) {
        assert(m->delay_time && m->now > m->delay_time);
        m->delay_seen = 1;
    }
    *p = value; /* Effect precedes an uncertain failure. */
    if (p == &m->host[4] && (value & 1)) m->busy = 2;
    ++m->writes;
    return finish(m);
}
static int barrier(void *ctx) { return finish(ctx); }
static int now_us(void *ctx, uint64_t *value)
{
    struct model *m = ctx;
    ++m->clocks;
    *value = m->now;
    return 1;
}
static void setup(struct a733_dsi_phy *p, struct model *m, unsigned controller,
                  unsigned lanes, uint32_t hs, uint32_t ls)
{
    memset(p, 0, sizeof(*p)); memset(m, 0, sizeof(*m));
    struct a733_dsi_pll_plan plan;
    assert(a733_dsi_pll_plan(hs, ls, &plan));
    phy_reference_factors(hs, ls, plan.factors);
    m->controller = controller; m->lanes = lanes; m->now = 1;
    m->authority = 1; m->driver = p;
    m->ccu[0] = 0x82000000; m->ccu[1] = 0x10001;
    m->phy[0x104 / 4] = plan.factors | 0x00700080;
    m->phy[0x108 / 4] = 0x00601000;
    m->phy[0xe0 / 4] = 1u << 26;
    /* Unrelated/reserved bits must survive masked analog and host writes. */
    m->phy[0x10 / 4] = 0x5a00;
    m->phy[0x4c / 4] = 0xa5000088;
    m->phy[0x5c / 4] = 0x08088800;
    m->host[0x10 / 4] = 0x80000000;
    m->host[0x20 / 4] = 0x8800;
    m->host[0x30 / 4] = 0x8800;
    memcpy(m->original_phy, m->phy, sizeof(m->phy));
    memcpy(m->original_host, m->host, sizeof(m->host));
    struct a733_dsi_phy_config c = {.ops = {admit, read32, write32, barrier, now_us},
        .context = m, .lease = 444, .deadline = 10000, .controller = controller,
        .lanes = lanes, .hs_hz = hs, .ls_hz = ls};
    assert(a733_dsi_phy_init(p, &c));
    memset(&c, 0xee, sizeof(c));
}
static enum a733_dsi_phy_state run(struct a733_dsi_phy *p, struct model *m)
{
    for (unsigned i = 0; i != 512; ++i) {
        unsigned before = m->calls;
        enum a733_dsi_phy_state state = a733_dsi_phy_poll(p);
        assert(m->calls - before <= 1);
        ++m->now;
        if (state != A733_DSI_PHY_PENDING) return state;
    }
    assert(!"unbounded PHY startup"); return A733_DSI_PHY_RETAINED;
}
static void quiet(struct a733_dsi_phy *p, struct model *m)
{
    unsigned calls = m->calls, clocks = m->clocks;
    enum a733_dsi_phy_state state = p->state;
    assert(a733_dsi_phy_poll(p) == state);
    assert(a733_dsi_phy_poll(p) == state);
    assert(calls == m->calls && clocks == m->clocks && p->config.lease == 444);
}
static int command_admit(void *ctx, unsigned controller, unsigned lanes, uint64_t lease)
{
    struct model *m = ctx;
    return m->authority && a733_dsi_phy_ready(m->driver, controller, lanes, lease);
}
static unsigned success(unsigned controller, unsigned lanes, uint32_t hs, uint32_t ls, int command)
{
    struct a733_dsi_phy p; struct model m;
    setup(&p, &m, controller, lanes, hs, ls);
    m.unlock = 1; /* A low lock sample must restart consecutive filtering. */
    assert(run(&p, &m) == A733_DSI_PHY_READY);
    assert(m.lock_reads == 8 && m.delay_seen);
    assert(m.now - m.lock_ready >= 20);
    phy_reference_compare(m.original_phy, m.phy, m.original_host, m.host, lanes);
    assert(a733_dsi_phy_ready(&p, controller, lanes, 444));
    assert(!a733_dsi_phy_ready(&p, controller, lanes, 445));
    assert(!a733_dsi_phy_ready(&p, controller ^ 1, lanes, 444));
    quiet(&p, &m);
    unsigned calls = m.calls;
    if (command) {
        struct a733_dsi d = {0};
        struct a733_dsi_config c = {.ops = {command_admit, read32, write32, barrier, now_us},
            .context = &m, .controller = controller, .lanes = lanes, .lease = 444};
        assert(a733_dsi_init(&d, &c));
        uint8_t payload = 0x11;
        assert(a733_dsi_submit(&d, 0x05, 0, &payload, 1, 10000));
        for (unsigned i = 0; i != 64 && d.state == A733_DSI_PENDING; ++i) {
            a733_dsi_poll(&d); ++m.now;
        }
        assert(d.state == A733_DSI_ENGINE_DONE);
    }
    return calls;
}
static void faults(void)
{
    struct a733_dsi_phy p; struct model m;
    const unsigned points[] = {1, 2, 25, 28, 49, 61, 92, 129, 131, 144};
    for (unsigned kind = 0; kind != 2; ++kind) {
        for (unsigned i = 0; i != sizeof(points)/sizeof(*points); ++i) {
            setup(&p, &m, 1, 4, 480000000, 120000000);
            if (kind) m.late_at = points[i]; else m.fail_at = points[i];
            assert(run(&p, &m) == A733_DSI_PHY_RETAINED);
            assert(m.calls == points[i]); quiet(&p, &m);
        }
    }
    for (unsigned i = 0; i != 8; ++i) {
        setup(&p, &m, 0, 4, 480000000, 120000000);
        switch (i) {
        case 0: m.phy[0x104 / 4] &= ~(1u << 20); break; /* Cold PLL unsupported. */
        case 1: m.phy[0x104 / 4] ^= 1; break;
        case 2: m.phy[0x10c / 4] |= 1u << 31; break;
        case 3: m.ccu[0] ^= 1; break;
        case 4: m.ccu[1] &= ~1u; break;
        case 5: m.host[0] |= 1; break;
        case 6: m.phy[0x110 / 4] |= 4; break;
        case 7: m.authority = 0; break;
        }
        assert(run(&p, &m) == A733_DSI_PHY_RETAINED && !m.writes);
        quiet(&p, &m);
    }
    setup(&p, &m, 0, 4, 480000000, 120000000);
    m.no_lock = 1; p.config.deadline = 100;
    assert(run(&p, &m) == A733_DSI_PHY_RETAINED && !m.writes);
    assert(p.error == A733_DSI_PHY_TIME);
    quiet(&p, &m);
    setup(&p, &m, 0, 4, 480000000, 120000000);
    m.corrupt_at = 29; /* First analog write readback. */
    assert(run(&p, &m) == A733_DSI_PHY_RETAINED && p.error == A733_DSI_PHY_VERIFY);
    quiet(&p, &m);
    setup(&p, &m, 0, 4, 480000000, 120000000);
    assert(a733_dsi_phy_poll(&p) == A733_DSI_PHY_PENDING);
    m.now = 0;
    assert(a733_dsi_phy_poll(&p) == A733_DSI_PHY_RETAINED && m.calls == 1);
    quiet(&p, &m);
}
static void admission_guards(void)
{
    const unsigned offsets[] = {0x54, 0x58, 0x58, 0x110, 0x10, 0x10};
    const uint32_t bits[] = {1, 1, 1u << 16, 1u << 4, 1u << 4, 1u << 12};
    for (unsigned i = 0; i != sizeof(offsets)/sizeof(*offsets); ++i) {
        struct a733_dsi_phy p; struct model m;
        setup(&p, &m, 0, 4, 480000000, 120000000);
        if (i < 4) m.phy[offsets[i] / 4] |= bits[i];
        else m.host[offsets[i] / 4] |= bits[i];
        assert(run(&p, &m) == A733_DSI_PHY_RETAINED);
        assert(p.error == A733_DSI_PHY_INITIAL_STATE && !m.writes);
        quiet(&p, &m);
    }
    puts("PASS six inherited manual/test-control admission guards before any write");
}
int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "admission")) {
        admission_guards(); return 0;
    }
    const uint32_t rates[][2] = {{480000000, 120000000}, {240000000, 60000000},
                                 {840000000, 140000000}, {960000000, 160000000}};
    for (unsigned i = 0; i != 4; ++i) success(i & 1, i + 1, rates[i][0], rates[i][1], i == 3);
    struct a733_dsi_pll_plan p, saved;
    memset(&p, 0x55, sizeof(p)); saved = p;
    const uint32_t bad[][2] = {{0, 1}, {48000000, 24000000}, {480000001, 120000000},
        {480000000, 120000001}, {1000000001, 1}, {480000000, 1200000001},
        {480000000, 1000000}, {840000000, 28000000}};
    for (unsigned i = 0; i != sizeof(bad)/sizeof(*bad); ++i) {
        assert(!a733_dsi_pll_plan(bad[i][0], bad[i][1], &p));
        assert(!memcmp(&p, &saved, sizeof(p)));
    }
    faults();
    admission_guards();
    puts("PASS inherited PLL plans, PHY/source images, delays, retained faults and command composition");
    return 0;
}
