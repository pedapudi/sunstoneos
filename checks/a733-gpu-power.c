/* GPU-local power sequencing with GPU_TOP (PCK domain 5) behind the row. A
 * stand-in for the power-domain owner answers the row from the modeled PPU
 * words before each controller poll: ON or OFF at once, never while stalled,
 * REFUSED when the domain is incoherent. checks/power-domains-pck.c runs the real
 * owner and writes the combined trace that a733-gpu-power-reference.py
 * compares with the pinned PCK functions. */
#include "a733_gpu_power.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct event { unsigned kind, bank, offset, mask, value; };
struct model {
    uint32_t regs[3][0x7000 / 4];
    struct event events[96];
    unsigned events_n, calls, fail_at, late_at, held, releases, writes;
    unsigned stalled, pending, status_reads, bad_rate, bad_supply, corrupt, partial;
    unsigned floored, slow, core_pending, core_stalled, core_reads, core_delayed, core_late;
    unsigned core_entry_reads, core_entry_fail, core_entry_late;
    /* When an outstanding CORE transition completes without clock/reset
     * progress: 0 never (only after progress), 1 at a status read once PWCR
     * is 0, 2 at a status read after a policy write with PWCR 0. */
    unsigned core_early, core_rewritten;
    uint64_t now, tick, gated, counter, gated_counter, settle_counts;
};
static struct model m;
static struct a733_gpu_power s;
static void row_notify(void *ctx) { assert(ctx == &m); }
/* The owner's answer, once per controller poll. */
static void answer(void)
{
    struct a733_power_domain_row *r = &s.row;
    const unsigned q = atomic_load(&r->request), g = (q >> 1) & A733_POWER_ROW_GENERATION_MASK, on = q & 1;
    if ((atomic_load(&r->state) >> 3) == g) return;
    uint32_t *policy = &m.regs[2][0x5000/4], *status = &m.regs[2][0x5008/4];
    if (!m.pending) {
        if ((*policy & 15) != (*status & 15) || ((*status & 15) != 0 && (*status & 15) != 8)) {
            atomic_store(&r->state, g << 3 | A733_POWER_ROW_REFUSED); return;
        }
        /* The owner rereads the GPU gate and reset closed before any command. */
        assert(!(m.regs[0][0xb24/4] & 0x10001) && !(m.regs[0][0xb20/4] & 0x80000000));
        *policy = (*policy & ~15u) | (on ? 8 : 0); m.pending = 1;
    }
    if (m.stalled) return;
    *status = (*status & ~15u) | (*policy & 15); m.pending = 0;
    atomic_store(&r->state, g << 3 | (on ? A733_POWER_ROW_ON : A733_POWER_ROW_OFF));
}
static enum a733_gpu_power_result poll(void) { answer(); return a733_gpu_power_poll(&s); }
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
    assert(!m.regs[2][0x6000/4] && !m.regs[2][0x6008/4]);
    if (!call()) return 0;
    m.held = 0; ++m.releases;
    return 1;
}
static int read_reg(void *ctx, enum a733_gpu_power_bank bank, uint32_t off, uint32_t *out)
{
    assert(ctx == &m && bank <= A733_GPU_CORE && off % 4 == 0 && off < 0x6000);
    if (!call()) return 0;
    if (bank == A733_GPU_CORE) {
        assert(off == 0 || off == 8 || off == 0x14 || off == 0x20 || off == 0xfb0);
        assert((m.regs[2][0x5008/4] & 15) == 8);
        /* Policy, status, IDR0, MISR and PWCR precede admission. */
        if ((s.core_seen & 0x1f) != 0x1f) {
            ++m.core_entry_reads;
            if (m.core_entry_reads == m.core_entry_fail) return 0;
            if (m.core_entry_reads == m.core_entry_late) m.now = s.deadline;
        }
        if (off == 8 && m.core_pending) {
            /* A counted target observation follows clock/reset progress for
             * ON and keeps those resources available for OFF. Earlier status
             * reads at ON are the diagnostic observations. */
            const int progressed = (m.regs[0][0xb20/4] & 0x80000000) &&
                                   (m.regs[0][0xb24/4] & 0x10001) == 0x10001;
            const int early = !m.core_stalled && m.regs[2][0x6020/4] == 0 &&
                              (m.core_early == 1 || (m.core_early == 2 && m.core_rewritten));
            if (progressed) {
                ++m.core_reads;
                if (m.core_late) m.now = s.deadline;
                if (m.core_delayed) --m.core_delayed;
                else if (!m.core_stalled) {
                    m.regs[2][0x6008/4] = m.regs[2][0x6000/4]; m.core_pending = 0;
                }
            } else {
                assert(s.phase == A733_GPU_POWER_STARTING);
                if (early) { m.regs[2][0x6008/4] = m.regs[2][0x6000/4]; m.core_pending = 0; }
            }
        }
        off += 0x6000;
    }
    *out = m.regs[bank][off/4];
    if (m.corrupt == off) *out ^= 1;
    return 1;
}
static int update(void *ctx, enum a733_gpu_power_bank bank, uint32_t off,
                  uint32_t clear, uint32_t set)
{
    assert(ctx == &m && (bank == A733_GPU_CCU || bank == A733_GPU_CORE) && off % 4 == 0 && off < 0x6000);
    if (!call()) return 0;
    if (bank == A733_GPU_CORE) {
        assert((m.regs[2][0x5008/4] & 15) == 8 && m.held);
        assert(clear == UINT32_MAX && ((off == 0 && (set == 0 || set == 8)) || (off == 0x20 && set == 0)));
        /* PWCR changes only in a settled state or the power-up state. */
        if (off == 0x20) assert(((m.regs[2][0x6000/4] == 0 || m.regs[2][0x6000/4] == 8) &&
                                 m.regs[2][0x6000/4] == m.regs[2][0x6008/4]) ||
                                (m.regs[2][0x6000/4] == 8 && m.regs[2][0x6008/4] == 0 &&
                                 m.regs[2][0x6020/4] == A733_GPU_CORE_RESET_PWCR));
        if (off == 0) m.core_pending = m.core_rewritten = 1;
        off += 0x6000;
    }
    event(1, bank, off, clear, set); ++m.writes; m.now += m.slow;
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
    m.regs[bank][off/4] = value;
    return !m.partial;
}
static const struct a733_gpu_power_ops ops = {now, acquire, release, read_reg, update};
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
    m.regs[2][0x6020/4] = 0x010101ff;
    m.regs[2][0x6014/4] = 0x00000100;
    m.regs[2][0x6fb0/4] = 0x00031001;
    assert(a733_gpu_power_init(&s, &ops, &m));
    s.row = (struct a733_power_domain_row){.domains = (struct a733_power_domains *)(void *)&m,
                                            .notify = row_notify, .notify_context = &m, .domain = 5};
}
static enum a733_gpu_power_result drain(void)
{
    enum a733_gpu_power_result r;
    unsigned n = 0;
    do { r = poll(); assert(++n < 1000); }
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
static void core_cases(void)
{
    for (unsigned top = 0; top < 2; ++top) for (unsigned core = 0; core < 2; ++core) {
        fresh(); m.regs[2][0x5000/4] |= top*8; m.regs[2][0x5008/4] |= top*8;
        m.regs[2][0x6000/4] = m.regs[2][0x6008/4] = core*8;
        m.regs[2][0x4000/4] = 0x12345678; m.regs[2][0x4008/4] = 0xabcdef08;
        on();
        assert(s.core_seen == 0xff && s.core_policy == core*8 && s.core_status == core*8 &&
               s.core_config == 0x010101ff && m.core_reads == 1);
        /* A settled entry reads each observation once, without a wait. */
        assert(s.core_note[A733_GPU_NOTE_IDR0] == 0x00031001 && s.core_note[A733_GPU_NOTE_MISR] == 0x100 &&
               s.core_note[A733_GPU_NOTE_AFTER_PWCR] == core*8 && s.core_note[A733_GPU_NOTE_AFTER_POLICY] == core*8 &&
               s.core_wait_us[0] <= 1 && s.core_wait_us[1] <= 1);
        off(); on(); off();
        assert(m.releases == 2 && m.regs[2][0x4000/4] == 0x12345678 &&
               m.regs[2][0x4008/4] == 0xabcdef08 && m.regs[0][0x5e0/4] == 0x50000);
    }
    /* Unsettled or unexpected entries, including policy 8 and status 0
     * with a PWCR other than the reset value, are refused before any write. */
    const uint32_t bad[][3] = {{8,0,0x010101ff},{8,0,0},{8,0,0x100},{0,8,0x101},{0x108,0x108,0x101},
                              {0x01000008,0x01000008,0x101},{0x1008,0x1008,0x101},{8,0x108,0x101}};
    for (unsigned i = 0; i < sizeof(bad)/sizeof(*bad); ++i) {
        fresh(); m.regs[2][0x6000/4] = bad[i][0]; m.regs[2][0x6008/4] = bad[i][1];
        m.regs[2][0x6020/4] = bad[i][2];
        assert(a733_gpu_power_on(&s,10000) == A733_GPU_POWER_AGAIN);
        assert(drain() == A733_GPU_POWER_FAULT && s.error == A733_GPU_POWER_STATE);
        assert(!m.writes && m.held && !m.releases && s.core_seen == 0x1f && s.index == 10 &&
               s.core_policy == bad[i][0] && s.core_status == bad[i][1] && s.core_config == bad[i][2] &&
               s.core_note[A733_GPU_NOTE_IDR0] == 0x00031001 && s.core_note[A733_GPU_NOTE_MISR] == 0x100);
        terminal();
    }
    /* Seen masks after each entry read: policy, status, IDR0, MISR, PWCR. */
    const uint32_t seen[] = {0, 1, 3, 0xb, 0x1b, 0x1f};
    for (unsigned late = 0; late < 2; ++late) for (unsigned sample = 1; sample <= 5; ++sample) {
        fresh(); if (late) m.core_entry_late = sample; else m.core_entry_fail = sample;
        assert(a733_gpu_power_on(&s,10000) == A733_GPU_POWER_AGAIN);
        assert(drain() == A733_GPU_POWER_FAULT && !m.writes && m.held && !m.releases);
        assert(m.core_entry_reads == sample && s.core_seen == seen[sample - !late]);
        terminal();
    }
    /* The power-up state: policy 8, status 0 and the reset PWCR, with the
     * default ON transition outstanding. It completes at a status read
     * after PWCR 0, after the policy write, only after clock/reset
     * progress, or never. Each observation after a write waits up to
     * A733_GPU_SPIN_US for status 8 and continues either way. */
    for (unsigned mode = 0; mode < 4; ++mode) {
        fresh(); m.regs[2][0x6000/4] = 8; m.regs[2][0x6020/4] = A733_GPU_CORE_RESET_PWCR;
        m.core_pending = 1; m.core_early = mode == 0 ? 1 : mode == 1 ? 2 : 0; m.core_stalled = mode == 3;
        assert(a733_gpu_power_on(&s, m.now + 1000) == A733_GPU_POWER_AGAIN);
        const enum a733_gpu_power_result r = drain();
        const uint32_t after_pwcr = mode == 0 ? 8 : 0, after_policy = mode <= 1 ? 8 : 0;
        assert(s.core_policy == 8 && s.core_status == 0 && s.core_config == A733_GPU_CORE_RESET_PWCR);
        assert(s.core_note[A733_GPU_NOTE_AFTER_PWCR] == after_pwcr &&
               s.core_note[A733_GPU_NOTE_AFTER_POLICY] == after_policy &&
               s.core_note[A733_GPU_NOTE_MISR_LATE] == 0x100);
        assert(after_pwcr ? s.core_wait_us[0] <= 1 : s.core_wait_us[0] >= A733_GPU_SPIN_US);
        assert(after_policy ? s.core_wait_us[1] <= 1 : s.core_wait_us[1] >= A733_GPU_SPIN_US);
        assert(s.core_wait_us[0] <= A733_GPU_SPIN_US + 2 && s.core_wait_us[1] <= A733_GPU_SPIN_US + 2);
        assert(!m.regs[2][0x6020/4] && m.regs[2][0x6000/4] == 8);
        if (mode < 3) {
            assert(r == A733_GPU_POWER_OK && s.core_seen == 0xff && m.regs[2][0x6008/4] == 8);
            assert(m.core_reads == (mode == 2));
            off(); assert(!m.regs[2][0x6000/4] && !m.regs[2][0x6008/4] && m.releases == 1);
            /* Reopening starts from settled OFF with PWCR 0. */
            on(); assert(s.core_policy == 0 && !s.core_config && s.core_wait_us[0] <= 1); off();
        } else {
            assert(r == A733_GPU_POWER_FAULT && s.error == A733_GPU_POWER_TIMEOUT && m.held && !m.releases);
            assert(s.index == 27 && !m.regs[2][0x6008/4]); /* the final status poll */
            terminal();
        }
    }
    /* Modes: acknowledgement on the third status read, inside one call's
     * spin; on the 81st read, after the first call's spin ends; never; and a
     * read that reaches the deadline. The clock advances 1 us per sample,
     * so one call makes at most A733_GPU_SPIN_US + 1 status reads. */
    for (unsigned stopping = 0; stopping < 2; ++stopping) for (unsigned mode = 0; mode < 4; ++mode) {
        fresh(); if (stopping) on();
        m.core_reads = 0;
        if (mode == 0) m.core_delayed = 2;
        if (mode == 1) m.core_delayed = 80;
        if (mode == 2) m.core_stalled = 1;
        if (mode == 3) m.core_late = 1;
        assert((stopping ? a733_gpu_power_off(&s,m.now+150) : a733_gpu_power_on(&s,m.now+150)) ==
               A733_GPU_POWER_AGAIN);
        enum a733_gpu_power_result r;
        unsigned polls = 0;
        do {
            const unsigned reads = m.core_reads;
            r = poll(); ++polls;
            assert(m.core_reads - reads <= A733_GPU_SPIN_US + 1 && polls < 200);
        } while (r == A733_GPU_POWER_AGAIN);
        if (mode < 2) {
            assert(r == A733_GPU_POWER_OK && m.core_reads == (mode ? 81 : 3));
            /* Each direction has one poll that waits for the TOP row. An
             * acknowledgement inside the spin adds no poll; a later one adds
             * one. */
            assert(polls == 2 + mode);
            if (!stopping) off();
        } else {
            assert(r == A733_GPU_POWER_FAULT && s.error == A733_GPU_POWER_TIMEOUT && m.held && !m.releases);
            assert((m.regs[2][0x5008/4] & 15) == 8 && (m.regs[0][0xb20/4] & 0x80000000) &&
                   (m.regs[0][0xb24/4] & 0x10001) == 0x10001);
            terminal();
        }
    }
    puts("PASS CORE6: four inherited pairs/reopen, eight entry refusals, ten partial samples, four power-up completions, eight bounded acknowledgement cases; NPU/shared gates retained");
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "--core")) { core_cases(); return 0; }
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
    fresh(); m.corrupt = 0xb20; assert(a733_gpu_power_on(&s, 10000) == A733_GPU_POWER_AGAIN);
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
    /* The first poll checks the preconditions and requests the row; it does
     * not wait. The poll after the owner's answer runs every remaining step
     * back to back, including 10 us register updates. A domain that has not
     * answered keeps the controller at the row step without spinning. */
    fresh(); m.slow = 10; assert(a733_gpu_power_on(&s, 10000) == A733_GPU_POWER_AGAIN);
    uint64_t entered = m.now;
    assert(a733_gpu_power_poll(&s) == A733_GPU_POWER_AGAIN && s.index == 5 && s.requested && !m.writes);
    assert(m.now - entered < A733_GPU_SPIN_US);
    answer(); assert(a733_gpu_power_poll(&s) == A733_GPU_POWER_OK && m.now - 100 >= 4 * 10); off();
    fresh(); m.stalled = 1; assert(a733_gpu_power_on(&s, 10000) == A733_GPU_POWER_AGAIN);
    assert(poll() == A733_GPU_POWER_AGAIN && s.index == 5);
    assert(poll() == A733_GPU_POWER_AGAIN && s.index == 5 && !m.writes);
    m.stalled = 0; assert(drain() == A733_GPU_POWER_OK); off();
    /* A clock that rounds counter ticks down to microseconds still separates
     * the clock gate and reset release by at least one microsecond. */
    fresh(); m.floored = 1; on(); off();
    unsigned settle = (unsigned)m.settle_counts; assert(settle >= 24);
    /* A stopped clock cannot end a wait: the poll returns AGAIN without
     * advancing the step or shortening the delay, and completes once the
     * clock runs again. */
    fresh(); m.tick = 0; assert(a733_gpu_power_on(&s, 10000) == A733_GPU_POWER_AGAIN);
    assert(poll() == A733_GPU_POWER_AGAIN && s.index == 5);
    assert(poll() == A733_GPU_POWER_AGAIN && s.index == 23);
    assert(!(m.regs[0][0xb24/4] & 0x10000) && poll() == A733_GPU_POWER_AGAIN);
    m.tick = 1; assert(drain() == A733_GPU_POWER_OK); off();
    fresh(); m.tick = 0; m.stalled = 1; assert(a733_gpu_power_on(&s, 10000) == A733_GPU_POWER_AGAIN);
    assert(poll() == A733_GPU_POWER_AGAIN && s.index == 5);
    m.tick = 1; m.stalled = 0; assert(drain() == A733_GPU_POWER_OK); off();
    /* An initially powered domain is acceptable only while reset/gates are safe. */
    fresh(); m.regs[2][0x5000/4] |= 8; m.regs[2][0x5008/4] |= 8; on(); off();
    /* A domain that the owner fails, rather than refuses, faults with DOMAIN. */
    fresh(); assert(a733_gpu_power_on(&s, 10000) == A733_GPU_POWER_AGAIN);
    assert(a733_gpu_power_poll(&s) == A733_GPU_POWER_AGAIN);
    atomic_store(&s.row.state, (atomic_load(&s.row.request) >> 1) << 3 | A733_POWER_ROW_FAILED);
    assert(a733_gpu_power_poll(&s) == A733_GPU_POWER_FAULT && s.error == A733_GPU_POWER_DOMAIN);
    terminal(); ++faults;
    /* Without a bound row the controller refuses to start. */
    fresh(); s.row.domains = 0; assert(a733_gpu_power_on(&s, 10000) == A733_GPU_POWER_INVALID);
    printf("PASS GPU power ownership, on/off/reopen, strict status, delay of %u counter ticks, stopped clock, %u retained faults (%u startup callbacks)\n",
           settle, faults, on_calls);
}
