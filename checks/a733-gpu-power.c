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
     * is 0. */
    unsigned core_early, core_rewritten;
    /* PLL_GPU0 (CCU 0x0e0): pll_ignore is the 1-based PLL write that does not
     * take; lock_after the reads with enable and lock enable set before the
     * lock bit reads 1 (UINT32_MAX never); lock_flicker drops the lock at the
     * second locked read; update_stuck keeps the 0xb20 update bit set. */
    unsigned pll_writes, pll_ignore, lock_after, lock_reads, lock_flicker, update_stuck;
    uint64_t locked_counter, output_counts;
    /* A status read after the PWCR write and before the policy write
     * returns this value when it is nonzero. */
    uint32_t core_after_pwcr_value;
    uint64_t now, tick, counter;
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
        /* The vendor order: domain 5 changes with the GPU clocked and out of
         * reset, PLL_GPU0 running, in both directions. */
        assert((m.regs[0][0xb24/4] & 0x10001) == 0x10001 && (m.regs[0][0xb20/4] & 0x80000000) &&
               (m.regs[0][0xe0/4] & 0xe8000000) == 0xe8000000);
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
        m.bad_rate ? 399999999u : 0,
        m.bad_supply ? A733_GPU_BOOT_UV : A733_GPU_SUPPLY_UV};
    return 1;
}
static int release(void *ctx)
{
    assert(ctx == &m && m.held);
    assert(!(m.regs[0][0xb20/4] & 0x80000000));
    assert(!(m.regs[0][0xb24/4] & 0x10001));
    assert((m.regs[2][0x5008/4] & 15) == 0);
    assert(!m.regs[2][0x6000/4] && !m.regs[2][0x6008/4]);
    assert(!(m.regs[0][0xe0/4] & 0xe8000000));
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
        /* Each ON starts with its policy read. */
        if (off == 0 && s.phase == A733_GPU_POWER_STARTING && !(s.core_seen & 1))
            m.core_rewritten = 0;
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
            const int early = !m.core_stalled && m.regs[2][0x6020/4] == 0 && m.core_early == 1;
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
        if (off == 8 && m.core_after_pwcr_value && !m.regs[2][0x6020/4] && !m.core_rewritten &&
            s.phase == A733_GPU_POWER_STARTING) {
            *out = m.core_after_pwcr_value; return 1;
        }
        off += 0x6000;
    }
    if (bank == A733_GPU_CCU && off == 0xe0) {
        /* The lock bit follows enable, LDO and lock enable. */
        uint32_t *pll = &m.regs[0][0xe0/4];
        if ((*pll & 0xe0000000) == 0xe0000000) {
            ++m.lock_reads;
            const int locked = m.lock_after != UINT32_MAX && m.lock_reads > m.lock_after &&
                               !(m.lock_flicker && m.lock_reads == m.lock_after + 2);
            *pll = locked ? *pll | (1u << 28) : *pll & ~(1u << 28);
            if (locked) m.locked_counter = m.counter;
        } else *pll &= ~(1u << 28);
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
        /* CORE6 policy changes only with TOP5 ON; PWCR is written before it. */
        assert(m.held && (off == 0x20 || (m.regs[2][0x5008/4] & 15) == 8));
        assert(clear == UINT32_MAX && ((off == 0 && (set == 0 || set == 8)) || (off == 0x20 && set == 0)));
        /* PWCR changes only in a settled state or the power-up state. */
        if (off == 0x20) assert(((m.regs[2][0x6000/4] == 0 || m.regs[2][0x6000/4] == 8) &&
                                 m.regs[2][0x6000/4] == m.regs[2][0x6008/4]) ||
                                (m.regs[2][0x6000/4] == 8 && m.regs[2][0x6008/4] == 0 &&
                                 m.regs[2][0x6020/4] == A733_GPU_CORE_RESET_PWCR));
        if (off == 0) m.core_pending = m.core_rewritten = 1;
        /* CORE6 policy 8 follows the clocks, and policy 0 precedes their
         * closing: the GPU is clocked at both CORE6 transitions. */
        if (off == 0) assert((m.regs[0][0xb20/4] & 0x80000000) && (m.regs[0][0xb24/4] & 0x10001) == 0x10001 &&
                             (m.regs[0][0xe0/4] & 0xf8000000) == 0xf8000000);
        off += 0x6000;
    }
    if (bank == A733_GPU_CCU && off == 0xe0) {
        assert(m.held);
        /* A 1008 MHz rate change needs the output gate closed, and the output
         * gate opens at least 20 us after the lock. */
        if (clear == 0x0070ff00) assert(!(m.regs[0][0xe0/4] & (1u << 27)));
        if (set == (1u << 27)) {
            assert((m.regs[0][0xe0/4] & 0xf0000000) == 0xf0000000);
            m.output_counts = m.counter - m.locked_counter;
            assert(!m.floored || m.output_counts >= 20 * 24);
        }
        if (++m.pll_writes == m.pll_ignore) { event(1, bank, off, clear, set); ++m.writes; return 1; }
        if (clear & 0xa0000000) m.lock_reads = 0;
    }
    event(1, bank, off, clear, set); ++m.writes; m.now += m.slow;
    uint32_t value = (m.regs[bank][off/4] & ~clear) | set;
    if (bank == A733_GPU_CCU && off == 0xb20) {
        assert(set & (1u << 27)); /* The update bit clears once the change is valid. */
        if (!m.update_stuck) value &= ~(1u << 27);
    }
    /* The vendor order: reset released before the bus gate, the bus gate
     * before the core gate, and the reset asserted after both gates close. */
    /* OFF closes the clocks only after TOP5 is OFF, as the vendor does. */
    if (bank == A733_GPU_CCU && s.phase == A733_GPU_POWER_STOPPING) assert(!(m.regs[2][0x5008/4] & 15));
    if (bank == A733_GPU_CCU && off == 0xb24 && set == 1) assert(m.regs[0][0xb24/4] & 0x10000);
    if (bank == A733_GPU_CCU && off == 0xb24 && clear == 0x10000) assert(!(m.regs[0][0xb24/4] & 1));
    if (bank == A733_GPU_CCU && off == 0xb20 && (set & 0x80000000)) assert(m.regs[0][0xb24/4] & 1);
    if (bank == A733_GPU_CCU && off == 0xe0 && (clear & 0x80000000)) assert(!(m.regs[0][0xb20/4] & 0x80000000));
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
    m.regs[0][0xe0/4] = 0x41104500; /* PLL_GPU0 as boot0 leaves it: disabled, 840 MHz factors */
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
        /* A settled entry reads each observation once. The policy write
         * follows the clocks, so the transition completes at its first
         * observation. Nothing writes the CORE6 delay row. */
        assert(s.core_note[A733_GPU_NOTE_IDR0] == 0x00031001 && s.core_note[A733_GPU_NOTE_MISR] == 0x100 &&
               s.core_note[A733_GPU_NOTE_AFTER_PWCR] == core*8 && s.core_note[A733_GPU_NOTE_AFTER_POLICY] == 8 &&
               !s.core_power_up);
        assert(!m.regs[2][0x6170/4] && !m.regs[2][0x6c10/4]);
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
     * default ON transition outstanding. It completes at the status read
     * after PWCR 0, only after clock/reset progress as the board showed, or
     * never. */
    for (unsigned mode = 0; mode < 3; ++mode) {
        fresh(); m.regs[2][0x6000/4] = 8; m.regs[2][0x6020/4] = A733_GPU_CORE_RESET_PWCR;
        m.core_pending = 1; m.core_early = mode == 0; m.core_stalled = mode == 2;
        assert(a733_gpu_power_on(&s, m.now + 1000) == A733_GPU_POWER_AGAIN);
        const enum a733_gpu_power_result r = drain();
        assert(s.core_policy == 8 && s.core_status == 0 && s.core_config == A733_GPU_CORE_RESET_PWCR &&
               s.core_power_up);
        assert(s.core_note[A733_GPU_NOTE_AFTER_PWCR] == (mode == 0 ? 8u : 0u));
        assert(s.core_note[A733_GPU_NOTE_AFTER_POLICY] == (mode < 2 ? 8u : 0u));
        assert(s.core_note[A733_GPU_NOTE_MISR_LATE] == 0x100);
        assert(!m.regs[2][0x6020/4] && m.regs[2][0x6000/4] == 8 && m.regs[0][0xe0/4] == 0xf9105300);
        if (mode < 2) {
            assert(r == A733_GPU_POWER_OK && s.core_seen == 0xff && m.regs[2][0x6008/4] == 8);
            assert(m.core_reads == 1);
            off(); assert(!m.regs[2][0x6000/4] && !m.regs[2][0x6008/4] && m.releases == 1 &&
                          m.regs[0][0xe0/4] == 0x01105300);
            /* Reopening starts from settled OFF with PWCR 0 and PLL_GPU0
             * disabled at its 1008 MHz factors. */
            on(); assert(s.core_policy == 0 && !s.core_config && !s.core_power_up &&
                         m.regs[0][0xe0/4] == 0xf9105300); off();
        } else {
            assert(r == A733_GPU_POWER_FAULT && s.error == A733_GPU_POWER_TIMEOUT && m.held && !m.releases);
            assert(s.index == 53 && !m.regs[2][0x6008/4]); /* the final status poll */
            terminal();
        }
    }
    /* Modes: acknowledgement on the third status read, inside one call's
     * spin; on the 81st read, after the first call's spin ends; never; and a
     * read that reaches the deadline. The clock advances 1 us per sample,
     * so one call makes at most A733_GPU_SPIN_US + 1 status reads in its
     * poll, after the one observation that follows the ON policy write. */
    for (unsigned stopping = 0; stopping < 2; ++stopping) for (unsigned mode = 0; mode < 4; ++mode) {
        fresh(); if (stopping) on();
        m.core_reads = 0;
        if (mode == 0) m.core_delayed = 2;
        if (mode == 1) m.core_delayed = 80;
        if (mode == 2) m.core_stalled = 1;
        if (mode == 3) m.core_late = 1;
        /* ON spends about 160 us in its clock steps before the poll. */
        assert((stopping ? a733_gpu_power_off(&s,m.now+150) : a733_gpu_power_on(&s,m.now+400)) ==
               A733_GPU_POWER_AGAIN);
        enum a733_gpu_power_result r;
        unsigned polls = 0;
        do {
            const unsigned reads = m.core_reads;
            r = poll(); ++polls;
            /* At ON the observation after the policy write adds one read. */
            assert(m.core_reads - reads <= A733_GPU_SPIN_US + 1 + !stopping && polls < 200);
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
    /* A status other than 0 or 8 at an observation fails with STATE, from
     * the power-up state and from settled OFF, after the PWCR write and
     * before any further write. */
    for (unsigned power_up = 0; power_up < 2; ++power_up) {
        fresh(); m.core_after_pwcr_value = 4;
        if (power_up) {
            m.regs[2][0x6000/4] = 8; m.regs[2][0x6020/4] = A733_GPU_CORE_RESET_PWCR; m.core_pending = 1;
        }
        assert(a733_gpu_power_on(&s, 10000) == A733_GPU_POWER_AGAIN);
        assert(drain() == A733_GPU_POWER_FAULT && s.error == A733_GPU_POWER_STATE);
        assert(s.index == 13 && s.core_power_up == power_up && s.last_value == 4 &&
               s.core_note[A733_GPU_NOTE_AFTER_PWCR] == 4 && (s.core_seen & 0x20));
        /* PWCR 0 only: no clock or policy write. */
        assert(m.writes == 1 && m.held && !m.releases && m.regs[2][0x6000/4] == power_up*8);
        terminal();
    }
    puts("PASS CORE6: four inherited pairs/reopen, eight entry refusals, ten partial samples, three power-up completions, two invalid observed statuses, eight bounded acknowledgement cases; NPU/shared gates retained");
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
    assert(m.regs[0][0xb20/4] == 0x90200100);
    assert(m.regs[0][0xb24/4] == 0x24690001);
    assert(m.regs[0][0xe0/4] == 0xf9105300 && s.pll_word == 0xf9105300);
    assert(s.pll_lock_reads[0] == 3 && s.pll_lock_reads[1] == 3 &&
           s.clock_update_reads[0] == 1 && s.clock_update_reads[1] == 1);
    assert(m.regs[2][0x5000/4] == 0xa5a50008);
    unsigned on_calls = m.calls;
    off();
    assert(s.phase == A733_GPU_POWER_OFF && !s.lease_held && !m.held && m.releases == 1);
    assert(m.regs[0][0xb20/4] == 0x10200100);
    assert(m.regs[0][0xb24/4] == 0x24680000);
    assert(m.regs[0][0xe0/4] == 0x01105300);
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
    /* No PERI0 parent is held or required: any reported parent rate passes. */
    fresh(); m.bad_rate = 1; on(); off();
    fresh(); m.bad_supply = 1; assert(a733_gpu_power_on(&s, 10000) == A733_GPU_POWER_AGAIN);
    assert(drain() == A733_GPU_POWER_FAULT && s.error == A733_GPU_POWER_RESOURCES);
    assert(m.writes == 0 && s.lease_held); terminal(); ++faults;
    /* Admission refusals, the last four for a GPU clock off its PLL_GPU0
     * source, a running PLL_GPU0, a PLL_GPU0 output left on and a PLL_GPU0
     * input divider M1 of 2, which the factor write would keep. */
    const unsigned bad_banks[] = {0, 0, 1, 0, 0, 2, 0, 0, 0, 0};
    const unsigned bad_offsets[] = {0x5c0, 0x5e0, 0x1ac, 0xb20, 0xb24, 0x5008, 0xb20, 0xe0, 0xe0, 0xe0};
    const unsigned bad_bits[] = {0x80, 0x10000, 1, 0x80000000, 0x10000, 1, 0x03000000, 0x80000000, 0x08000000, 2};
    for (unsigned i = 0; i < 10; ++i) {
        fresh(); m.regs[bad_banks[i]][bad_offsets[i]/4] ^= bad_bits[i];
        assert(a733_gpu_power_on(&s, 10000) == A733_GPU_POWER_AGAIN);
        assert(drain() == A733_GPU_POWER_FAULT && s.error == A733_GPU_POWER_STATE);
        /* An incoherent TOP5, which the owner refuses, stops at the row step
         * with the GPU clocked and CORE6 unchanged; every other refusal comes
         * before any write. */
        if (bad_offsets[i] == 0x5008)
            assert(s.index == 48 && m.writes == 15 && (m.regs[0][0xb24/4] & 0x10001) == 0x10001 &&
                   !(m.regs[2][0x6000/4] & 8));
        else assert(m.writes == 0);
        terminal(); ++faults;
    }
    fresh(); m.corrupt = 0xb20; assert(a733_gpu_power_on(&s, 10000) == A733_GPU_POWER_AGAIN);
    assert(drain() == A733_GPU_POWER_FAULT && s.error == A733_GPU_POWER_STATE);
    terminal(); ++faults;
    /* Each of the ten PLL_GPU0 writes of an ON that does not take fails
     * with PLL at its read-back, before the CORE6 policy write and with no
     * further access. The first two find their bit already as written
     * (output gate clear, LDO set at reset), so ignoring them changes
     * nothing. */
    unsigned pll_cases = 0;
    for (unsigned w = 1; w <= 10; ++w) {
        fresh(); m.pll_ignore = w; assert(a733_gpu_power_on(&s, 10000) == A733_GPU_POWER_AGAIN);
        if (w <= 2) { assert(drain() == A733_GPU_POWER_OK); off(); continue; }
        assert(drain() == A733_GPU_POWER_FAULT && s.error == A733_GPU_POWER_PLL && s.lease_held);
        assert(!m.regs[2][0x6000/4]); terminal(); ++faults; ++pll_cases;
    }
    /* A PLL that never locks fails with LOCK after 100 reads; a late lock
     * and a lock that drops once complete, counting the reads. */
    fresh(); m.lock_after = UINT32_MAX; assert(a733_gpu_power_on(&s, 100000) == A733_GPU_POWER_AGAIN);
    assert(drain() == A733_GPU_POWER_FAULT && s.error == A733_GPU_POWER_LOCK && s.pll_lock_reads[0] == 100 &&
           s.index == 22 && !(m.regs[0][0xb24/4] & 0x10001) && !(m.regs[2][0x5000/4] & 15)); terminal(); ++faults;
    fresh(); m.lock_after = 10; on();
    assert(s.pll_lock_reads[0] == 12 && s.pll_lock_reads[1] == 12); off();
    fresh(); m.lock_flicker = 1; on();
    assert(s.pll_lock_reads[0] == 4 && s.pll_lock_reads[1] == 4); off();
    /* A GPU clock update bit that stays set fails with UPDATE. */
    fresh(); m.update_stuck = 1; assert(a733_gpu_power_on(&s, 100000) == A733_GPU_POWER_AGAIN);
    assert(drain() == A733_GPU_POWER_FAULT && s.error == A733_GPU_POWER_UPDATE &&
           s.clock_update_reads[0] == 100 && s.index == 31 && !(m.regs[2][0x5000/4] & 15)); terminal(); ++faults;
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
    /* The first write, PWCR 0, reports failure before TOP5 or any clock. */
    assert(s.index == 11 && !(m.regs[2][0x5000/4] & 15) && s.lease_held);
    terminal(); ++faults;
    fresh(); on(); m.late_at = all_calls - 1;
    assert(a733_gpu_power_off(&s, 10000) == A733_GPU_POWER_AGAIN);
    assert(drain() == A733_GPU_POWER_FAULT && s.error == A733_GPU_POWER_TIMEOUT);
    assert(!s.lease_held && m.releases == 1); terminal(); ++faults;
    /* The first poll runs the admission, PWCR 0 and the 15 clock writes, then
     * requests the row; it does not wait for it. The poll after the owner's
     * answer runs the remaining steps. A domain that has not answered keeps
     * the controller at the row step, with the GPU clocked, without
     * spinning. */
    fresh(); m.slow = 10; assert(a733_gpu_power_on(&s, 10000) == A733_GPU_POWER_AGAIN);
    assert(a733_gpu_power_poll(&s) == A733_GPU_POWER_AGAIN && s.index == 48 && s.requested && m.writes == 15);
    answer(); assert(a733_gpu_power_poll(&s) == A733_GPU_POWER_OK && m.writes == 16); off();
    fresh(); m.stalled = 1; assert(a733_gpu_power_on(&s, 10000) == A733_GPU_POWER_AGAIN);
    assert(poll() == A733_GPU_POWER_AGAIN && s.index == 48);
    assert(poll() == A733_GPU_POWER_AGAIN && s.index == 48 && m.writes == 15);
    m.stalled = 0; assert(drain() == A733_GPU_POWER_OK); off();
    /* A clock that rounds counter ticks down to microseconds still opens the
     * PLL_GPU0 output at least 20 us after the lock. */
    fresh(); m.floored = 1; on(); off();
    unsigned settle = (unsigned)m.output_counts; assert(settle >= 20 * 24);
    /* A stopped clock cannot end a wait: the poll returns AGAIN without
     * advancing the step or shortening the delay, and completes once the
     * clock runs again. */
    fresh(); m.tick = 0; assert(a733_gpu_power_on(&s, 10000) == A733_GPU_POWER_AGAIN);
    assert(poll() == A733_GPU_POWER_AGAIN && s.index == 22);
    assert(!(m.regs[0][0xb24/4] & 0x10000) && poll() == A733_GPU_POWER_AGAIN && s.index == 22);
    m.tick = 1; assert(drain() == A733_GPU_POWER_OK); off();
    fresh(); m.tick = 0; m.stalled = 1; assert(a733_gpu_power_on(&s, 10000) == A733_GPU_POWER_AGAIN);
    assert(poll() == A733_GPU_POWER_AGAIN && s.index == 22);
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
    printf("PASS GPU power ownership, vendor clock order, on/off/reopen, strict status, PLL output %u counter ticks after lock, "
           "stopped clock, %u retained faults (%u ignored PLL writes, %u startup callbacks)\n",
           settle, faults, pll_cases, on_calls);
}
