#include "a733_gpu_power.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct event { unsigned kind, bank, offset, mask, value; };
struct model {
    uint32_t regs[3][0x6000 / 4];
    struct event events[96];
    unsigned events_n, calls, fail_at, late_at, held, releases, writes;
    unsigned stalled, pending, status_reads, bad_rate, bad_supply, corrupt, partial;
    unsigned floored, slow;
    uint64_t now, tick, gated, counter, gated_counter, settle_counts;
};
static struct model m;
static struct a733_gpu_power s;
/* With floored set, every callback takes one 24 MHz counter tick and the
 * clock reports whole microseconds rounded down, as the board clock does. */
static int call(void)
{
    ++m.calls; ++m.counter;
    if (m.calls == m.late_at) m.now = 100000;
    return m.calls != m.fail_at;
}
static void event(unsigned kind, unsigned bank, unsigned off, unsigned mask, unsigned value)
{
    assert(m.events_n < 96);
    m.events[m.events_n++] = (struct event){kind, bank, off, mask, value};
}
static int now(void *ctx, uint64_t *out)
{
    assert(ctx == &m); if (!call()) return 0;
    *out = m.floored ? 100 + m.counter / 24 : m.now; m.now += m.tick; return 1;
}
static int acquire(void *ctx, struct a733_gpu_power_resources *out)
{
    assert(ctx == &m && !m.held);
    if (!call()) return 0;
    m.held = 1;
    *out = (struct a733_gpu_power_resources){
        m.bad_rate ? 399999999u : A733_GPU_INITIAL_HZ,
        m.bad_supply ? 799999u : A733_GPU_INITIAL_UV};
    return 1;
}
static int release(void *ctx)
{
    assert(ctx == &m && m.held);
    assert(!(m.regs[0][0xb20/4] & 0x80000000));
    assert(!(m.regs[0][0xb24/4] & 0x10001));
    assert((m.regs[2][0x5008/4] & 15) == 0);
    if (!call()) return 0;
    m.held = 0; ++m.releases;
    return 1;
}
static int read_reg(void *ctx, enum a733_gpu_power_bank bank, uint32_t off, uint32_t *out)
{
    assert(ctx == &m && bank <= A733_GPU_PCK && off % 4 == 0 && off < 0x6000);
    if (!call()) return 0;
    if (bank == A733_GPU_PCK && off == 0x5008 && m.pending) {
        if (++m.status_reads == 3 && !m.stalled) {
            m.regs[bank][off/4] = (m.regs[bank][off/4] & ~15u) |
                                (m.regs[bank][0x5000/4] & 15);
            m.pending = 0;
        }
    }
    *out = m.regs[bank][off/4];
    if (m.corrupt == off) *out ^= 1;
    return 1;
}
static int write_reg(void *ctx, enum a733_gpu_power_bank bank, uint32_t off, uint32_t value)
{
    assert(ctx == &m && bank == A733_GPU_PCK && off % 4 == 0 && off < 0x6000);
    if (!call()) return 0;
    event(0, bank, off, 0, value); ++m.writes;
    m.regs[bank][off/4] = value; m.now += m.slow;
    return 1;
}
static int update(void *ctx, enum a733_gpu_power_bank bank, uint32_t off,
                  uint32_t clear, uint32_t set)
{
    assert(ctx == &m && bank <= A733_GPU_PCK && off % 4 == 0 && off < 0x6000);
    if (!call()) return 0;
    event(1, bank, off, clear, set); ++m.writes;
    uint32_t value = (m.regs[bank][off/4] & ~clear) | set;
    if (bank == A733_GPU_CCU && off == 0xb20) {
        assert(set & (1u << 27)); /* The update key reads as zero. */
        value &= ~(1u << 27);
    }
    /* Reset release follows the clock gate by the 1 us settling delay. */
    if (bank == A733_GPU_CCU && off == 0xb24 && set == 1) { m.gated = m.now; m.gated_counter = m.counter; }
    if (bank == A733_GPU_CCU && off == 0xb24 && set == 0x10000) {
        assert(m.now >= m.gated + 2 && (!m.floored || m.counter - m.gated_counter >= 24));
        m.settle_counts = m.counter - m.gated_counter;
    }
    if (bank == A733_GPU_PCK && off == 0x5000) {
        assert(!(m.regs[0][0xb24/4] & 0x10001));
        assert(!(m.regs[0][0xb20/4] & 0x80000000));
        m.pending = 1; m.status_reads = 0;
    }
    m.regs[bank][off/4] = value;
    return !m.partial;
}
static const struct a733_gpu_power_ops ops = {now, acquire, release, read_reg, write_reg, update};
static void fresh(void)
{
    memset(&m, 0, sizeof(m)); memset(&s, 0, sizeof(s));
    m.now = 100; m.tick = 1;
    m.regs[0][0x5c0/4] = 0x04000080;
    m.regs[0][0x5e0/4] = 0x00050000;
    m.regs[1][0x1ac/4] = 0x00400001;
    m.regs[0][0xb20/4] = 0x10200100;
    m.regs[0][0xb24/4] = 0x24680000;
    m.regs[2][0x5000/4] = 0xa5a50000;
    m.regs[2][0x5008/4] = 0x42000000;
    assert(a733_gpu_power_init(&s, &ops, &m));
}
static enum a733_gpu_power_result drain(void)
{
    enum a733_gpu_power_result r;
    unsigned n = 0;
    do { r = a733_gpu_power_poll(&s); assert(++n < 1000); }
    while (r == A733_GPU_POWER_AGAIN);
    return r;
}
static void terminal(void)
{
    unsigned calls = m.calls;
    assert(a733_gpu_power_poll(&s) == A733_GPU_POWER_FAULT);
    assert(a733_gpu_power_on(&s, 10000) == A733_GPU_POWER_INVALID);
    assert(a733_gpu_power_off(&s, 10000) == A733_GPU_POWER_INVALID);
    assert(a733_gpu_power_cancel(&s) == A733_GPU_POWER_INVALID);
    assert(calls == m.calls);
}
static void on(void)
{ assert(a733_gpu_power_on(&s, 10000) == A733_GPU_POWER_AGAIN); assert(drain() == A733_GPU_POWER_OK); }
static void off(void)
{ assert(a733_gpu_power_off(&s, 10000) == A733_GPU_POWER_AGAIN); assert(drain() == A733_GPU_POWER_OK); }
int main(int argc, char **argv)
{
    assert(argc == 2);
    fresh(); assert(!a733_gpu_power_init(&s, &ops, &m));
    assert(a733_gpu_power_off(&s, 10000) == A733_GPU_POWER_INVALID);
    assert(a733_gpu_power_poll(&s) == A733_GPU_POWER_INVALID);
    on();
    assert(s.phase == A733_GPU_POWER_ON && s.lease_held && m.held);
    assert(m.regs[0][0xb20/4] == 0x93200100);
    assert(m.regs[0][0xb24/4] == 0x24690001);
    assert(m.regs[2][0x5000/4] == 0xa5a50008);
    unsigned on_calls = m.calls;
    off();
    assert(s.phase == A733_GPU_POWER_OFF && !s.lease_held && !m.held && m.releases == 1);
    assert(m.regs[0][0xb20/4] == 0x13200100);
    assert(m.regs[0][0xb24/4] == 0x24680000);
    assert(m.regs[2][0x5000/4] == 0xa5a50000);
    assert(m.regs[0][0x5c0/4] == 0x04000080 && m.regs[0][0x5e0/4] == 0x00050000);
    assert(m.regs[1][0x1ac/4] == 0x00400001);
    unsigned all_calls = m.calls;
    FILE *f = fopen(argv[1], "w"); assert(f);
    for (unsigned i = 0; i < m.events_n; ++i) {
        struct event *e = &m.events[i];
        fprintf(f, "%u %u %x %x %x\n", e->kind, e->bank, e->offset, e->mask, e->value);
    }
    assert(!fclose(f));
    on(); off(); assert(m.releases == 2);
    unsigned faults = 0;
    /* Sweep callback errors along one successful on/off operation. */
    for (unsigned i = 1; i <= all_calls; ++i) {
        fresh(); m.fail_at = i;
        enum a733_gpu_power_result r = a733_gpu_power_on(&s, 10000);
        if (r == A733_GPU_POWER_AGAIN) r = drain();
        if (r == A733_GPU_POWER_OK) {
            r = a733_gpu_power_off(&s, 10000);
            if (r == A733_GPU_POWER_AGAIN) r = drain();
        }
        assert(r == A733_GPU_POWER_FAULT && s.error == A733_GPU_POWER_IO);
        terminal(); ++faults;
    }
    for (unsigned mode = 0; mode < 2; ++mode) {
        fresh(); if (mode) on();
        m.stalled = 1;
        assert((mode ? a733_gpu_power_off(&s, m.now + 150) :
                a733_gpu_power_on(&s, m.now + 150)) == A733_GPU_POWER_AGAIN);
        assert(drain() == A733_GPU_POWER_FAULT && s.error == A733_GPU_POWER_TIMEOUT);
        assert(!m.releases && s.lease_held); terminal(); ++faults;
    }
    fresh(); m.bad_rate = 1; assert(a733_gpu_power_on(&s, 10000) == A733_GPU_POWER_AGAIN);
    assert(drain() == A733_GPU_POWER_FAULT && s.error == A733_GPU_POWER_RESOURCES);
    assert(m.writes == 0 && s.lease_held); terminal(); ++faults;
    fresh(); m.bad_supply = 1; assert(a733_gpu_power_on(&s, 10000) == A733_GPU_POWER_AGAIN);
    assert(drain() == A733_GPU_POWER_FAULT && s.error == A733_GPU_POWER_RESOURCES);
    assert(m.writes == 0 && s.lease_held); terminal(); ++faults;
    const unsigned bad_banks[] = {0, 0, 1, 0, 0, 2};
    const unsigned bad_offsets[] = {0x5c0, 0x5e0, 0x1ac, 0xb20, 0xb24, 0x5008};
    const unsigned bad_bits[] = {0x80, 0x10000, 1, 0x80000000, 0x10000, 1};
    for (unsigned i = 0; i < 6; ++i) {
        fresh(); m.regs[bad_banks[i]][bad_offsets[i]/4] ^= bad_bits[i];
        assert(a733_gpu_power_on(&s, 10000) == A733_GPU_POWER_AGAIN);
        assert(drain() == A733_GPU_POWER_FAULT && s.error == A733_GPU_POWER_STATE);
        assert(m.writes == 0); terminal(); ++faults;
    }
    fresh(); m.corrupt = 0x5174; assert(a733_gpu_power_on(&s, 10000) == A733_GPU_POWER_AGAIN);
    assert(drain() == A733_GPU_POWER_FAULT && s.error == A733_GPU_POWER_STATE);
    terminal(); ++faults;
    fresh(); m.late_at = 4; assert(a733_gpu_power_on(&s, 10000) == A733_GPU_POWER_AGAIN);
    assert(drain() == A733_GPU_POWER_FAULT && s.error == A733_GPU_POWER_TIMEOUT);
    assert(s.lease_held && m.writes == 0); terminal(); ++faults;
    fresh(); assert(a733_gpu_power_on(&s, 10000) == A733_GPU_POWER_AGAIN); m.now = 0;
    assert(drain() == A733_GPU_POWER_FAULT && s.error == A733_GPU_POWER_CLOCK);
    terminal(); ++faults;
    fresh(); assert(a733_gpu_power_on(&s, 10000) == A733_GPU_POWER_AGAIN);
    assert(a733_gpu_power_cancel(&s) == A733_GPU_POWER_FAULT); terminal(); ++faults;
    fresh(); m.partial = 1; assert(a733_gpu_power_on(&s, 10000) == A733_GPU_POWER_AGAIN);
    assert(drain() == A733_GPU_POWER_FAULT && s.error == A733_GPU_POWER_IO);
    assert((m.regs[2][0x5000/4] & 15) == 8 && s.lease_held);
    terminal(); ++faults;
    fresh(); on(); m.late_at = all_calls - 1;
    assert(a733_gpu_power_off(&s, 10000) == A733_GPU_POWER_AGAIN);
    assert(drain() == A733_GPU_POWER_FAULT && s.error == A733_GPU_POWER_TIMEOUT);
    assert(!s.lease_held && m.releases == 1); terminal(); ++faults;
    /* Steps run back to back: one poll after begin powers on when the PCK
     * acknowledges within the spin bound. A slower acknowledgement returns
     * AGAIN after the bound and completes on a later poll. */
    fresh(); assert(a733_gpu_power_on(&s, 10000) == A733_GPU_POWER_AGAIN);
    assert(a733_gpu_power_poll(&s) == A733_GPU_POWER_OK); off();
    fresh(); m.stalled = 1; assert(a733_gpu_power_on(&s, 10000) == A733_GPU_POWER_AGAIN);
    uint64_t entered = m.now;
    assert(a733_gpu_power_poll(&s) == A733_GPU_POWER_AGAIN && s.index == 17);
    assert(m.now - entered >= A733_GPU_POWER_SPIN_US && m.now - entered <= 3 * A733_GPU_POWER_SPIN_US);
    m.stalled = 0; m.status_reads = 0; assert(drain() == A733_GPU_POWER_OK); off();
    /* Earlier steps in a poll do not consume the acknowledgement's re-read
     * budget: with 10 us register writes, one poll still completes startup. */
    fresh(); m.slow = 10; assert(a733_gpu_power_on(&s, 10000) == A733_GPU_POWER_AGAIN);
    assert(a733_gpu_power_poll(&s) == A733_GPU_POWER_OK && m.now - 100 >= 6 * 10); off();
    /* A clock that rounds counter ticks down to microseconds still separates
     * the clock gate and reset release by at least one microsecond. */
    fresh(); m.floored = 1; on(); off();
    unsigned settle = (unsigned)m.settle_counts; assert(settle >= 24);
    /* A stopped clock cannot end a wait: the poll returns AGAIN without
     * advancing the step or shortening the delay, and completes once the
     * clock runs again. */
    fresh(); m.tick = 0; assert(a733_gpu_power_on(&s, 10000) == A733_GPU_POWER_AGAIN);
    assert(a733_gpu_power_poll(&s) == A733_GPU_POWER_AGAIN && s.index == 24);
    assert(!(m.regs[0][0xb24/4] & 0x10000) && a733_gpu_power_poll(&s) == A733_GPU_POWER_AGAIN);
    m.tick = 1; assert(drain() == A733_GPU_POWER_OK); off();
    fresh(); m.tick = 0; m.stalled = 1; assert(a733_gpu_power_on(&s, 10000) == A733_GPU_POWER_AGAIN);
    assert(a733_gpu_power_poll(&s) == A733_GPU_POWER_AGAIN && s.index == 17);
    m.tick = 1; m.stalled = 0; m.status_reads = 0; assert(drain() == A733_GPU_POWER_OK); off();
    /* An initially powered domain is acceptable only while reset/gates are safe. */
    fresh(); m.regs[2][0x5008/4] |= 8; on(); off();
    printf("PASS GPU power ownership, on/off/reopen, strict status, delay of %u counter ticks, stopped clock, %u retained faults (%u startup callbacks)\n",
           settle, faults, on_calls);
}
