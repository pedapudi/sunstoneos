/* Returning CPU0 interval: production local GIC setup, cached EL1 entry,
 * and restoration of its initially awake interrupt configuration. */
#include "a733_cpu_continue.h"
#include "secondary-local-probe.h"

#define RDIST UINT32_C(0x03460000)
#define SGI (RDIST + UINT32_C(0x10000))
struct primary_local_record {
    uint64_t state, epoch, deadline, phase, error, writes;
    uint64_t sre_before, group0, group1, pmr;
    struct secondary_local_state before, after;
    uint64_t cleanup_error, group0_after, group1_after, pmr_after;
    uint64_t reserved[30];
};
_Static_assert(sizeof(struct primary_local_record) == 512, "Primary report ABI");
static volatile struct primary_local_record *const report = (void *)0x42018a00;
extern int native_el3_cache_probe(uint64_t epoch, uint64_t deadline);

static void barrier(void) { __asm__ volatile("dsb sy" ::: "memory"); }
static uint64_t ticks(void)
{ uint64_t n; __asm__ volatile("isb\n mrs %0,cntpct_el0" : "=r"(n) :: "memory"); return n; }
static uint32_t read_word(uintptr_t address)
{ barrier(); uint32_t n = *(volatile uint32_t *)address; barrier(); return n; }
static void write_word(uintptr_t address, uint32_t value)
{ barrier(); *(volatile uint32_t *)address = value; barrier(); }
static int now_us(void *context, uint64_t *out)
{ (void)context; *out = ticks() / 24; return 1; }
static int read32(void *context, uintptr_t address, uint32_t *out)
{
    (void)context;
    if (address != 0x03400000 && address != RDIST && address != RDIST + 0x14 &&
        address != RDIST + 0x24 && address != SGI + 0x300 && address != SGI + 0x80 &&
        address != SGI + 0xd00 && address != SGI + 0xc04 &&
        address != SGI + 0x400 && address != SGI + 0x100) return 0;
    *out = read_word(address); return 1;
}
static int read64(void *context, uintptr_t address, uint64_t *out)
{
    (void)context;
    if (address < 0x03460008 || address > 0x03540008 ||
        ((address - 0x03460008) & 0x1ffff)) return 0;
    barrier(); *out = *(volatile uint64_t *)address; barrier(); return 1;
}
static int write32(void *context, uintptr_t address, uint32_t value)
{
    (void)context;
    if (address != RDIST + 0x14 && address != RDIST + 0x24 &&
        address != SGI + 0x180 && address != SGI + 0x280 && address != SGI + 0x380 &&
        address != SGI + 0x80 && address != SGI + 0xd00 && address != SGI + 0xc04 &&
        address != SGI + 0x400 && address != SGI + 0x100) return 0;
    write_word(address, value); return 1;
}
static int wait_rwp(void)
{
    uint64_t previous = ticks();
    do {
        uint64_t now = ticks();
        if (now < previous || now >= report->deadline) return 0;
        previous = now;
        if (!(read_word(RDIST) & 8)) return 1;
    } while (1);
}
static void snapshot(volatile struct secondary_local_state *s)
{
    s->ctlr = read_word(RDIST); s->waker = read_word(RDIST + 0x14);
    s->enable = read_word(SGI + 0x100); s->pending = read_word(SGI + 0x200);
    s->active = read_word(SGI + 0x300); s->group = read_word(SGI + 0x80);
    s->modifier = read_word(SGI + 0xd00); s->configuration = read_word(SGI + 0xc04);
    s->priority = read_word(SGI + 0x400); s->pwrr = read_word(RDIST + 0x24);
}
static int restore_local(void)
{
    uint64_t g0, g1;
    __asm__ volatile("mrs %0,icc_igrpen0_el1\n mrs %1,icc_igrpen1_el3" : "=r"(g0), "=r"(g1));
    if ((g0 & 1) || (g1 & 3)) { report->cleanup_error = 1; return 0; }
    write_word(SGI + 0x180, UINT32_MAX);
    if (!wait_rwp() || read_word(SGI + 0x100) || read_word(SGI + 0x200) ||
        read_word(SGI + 0x300)) { report->cleanup_error = 2; return 0; }
    const volatile struct secondary_local_state *b = &report->before;
    write_word(SGI + 0x80, b->group); write_word(SGI + 0xd00, b->modifier);
    write_word(SGI + 0xc04, b->configuration); write_word(SGI + 0x400, b->priority);
    /* CPU0 was awake and remains awake. Do not apply secondary sleep/OFF cleanup. */
    snapshot(&report->after);
    const volatile uint64_t *before = (const volatile uint64_t *)b;
    const volatile uint64_t *after = (const volatile uint64_t *)&report->after;
    for (unsigned i = 0; i < sizeof(*b) / 8; ++i)
        if (before[i] != after[i]) { report->cleanup_error = 3; return 0; }
    return 1;
}
int primary_local_cache_probe(uint64_t epoch, uint64_t deadline)
{
    uint64_t mpidr, el, daif, frequency; unsigned core;
    __asm__ volatile("mrs %0,mpidr_el1\n mrs %1,CurrentEL\n mrs %2,daif\n mrs %3,cntfrq_el0"
        : "=r"(mpidr), "=r"(el), "=r"(daif), "=r"(frequency));
    if (!epoch || el != 12 || (daif & 0x3c0) != 0x3c0 || frequency != 24000000 ||
        !a733_cpu_index(mpidr, &core) || core || ticks() >= deadline) return 0;
    const volatile uint64_t *words = (const volatile uint64_t *)report;
    for (unsigned i = 0; i < sizeof(*report) / 8; ++i) if (words[i]) return 0;
    report->state = 1; report->epoch = epoch; report->deadline = deadline;
    uint32_t power = read_word(RDIST + 0x24);
    if (power & 1 || (((power >> 2) ^ (power >> 3)) & 1)) {
        report->before.pwrr = power; report->error = 0x1001;
        barrier(); report->state = 4; barrier(); return 2;
    }
    snapshot(&report->before);
    if (report->before.ctlr || report->before.waker || report->before.enable ||
        report->before.pending || report->before.active || report->before.pwrr != power) {
        report->error = 0x1002; barrier(); report->state = 4; barrier(); return 2;
    }
    uint64_t sre, g0, g1, pmr;
    __asm__ volatile("mrs %0,icc_sre_el3" : "=r"(sre)); report->sre_before = sre;
    sre |= 15;
    __asm__ volatile("msr icc_sre_el3,%0\n isb" :: "r"(sre) : "memory");
    __asm__ volatile("mrs %0,icc_sre_el3" : "=r"(sre)); if ((sre & 15) != 15) return 0;
    __asm__ volatile("mrs %0,icc_igrpen0_el1\n mrs %1,icc_igrpen1_el3\n mrs %2,icc_pmr_el1"
        : "=r"(g0), "=r"(g1), "=r"(pmr));
    report->group0 = g0; report->group1 = g1; report->pmr = pmr;
    __asm__ volatile("msr icc_igrpen0_el1,xzr\n msr icc_igrpen1_el3,xzr\n isb" ::: "memory");
    if (!secondary_distributor_begin(deadline)) return 0;
    struct a733_cpu_continue state = {0};
    struct a733_cpu_continue_config config = {.ops = {read32, read64, write32, now_us},
        .epoch = epoch, .deadline_us = deadline / 24, .mpidr = mpidr};
    if (!a733_cpu_continue_init(&state, &config)) return 0;
    while (a733_cpu_continue_poll(&state) == A733_CONTINUE_PENDING) {}
    report->phase = state.phase; report->error = state.error; report->writes = state.writes_attempted;
    int ready = state.state == A733_CONTINUE_GIC_READY && state.redistributor == RDIST;
    if (!ready && !state.error) return 0;
    report->state = 2; barrier();
    if (ready && !native_el3_cache_probe(epoch, deadline)) return 0;
    if (!restore_local() || !secondary_distributor_restore(deadline)) return 0;
    __asm__ volatile("msr icc_pmr_el1,%0\n msr icc_igrpen0_el1,%1\n msr icc_igrpen1_el3,%2\n isb"
        :: "r"(pmr), "r"(g0), "r"(g1) : "memory");
    __asm__ volatile("mrs %0,icc_pmr_el1\n mrs %1,icc_igrpen0_el1\n mrs %2,icc_igrpen1_el3"
        : "=r"(pmr), "=r"(g0), "=r"(g1));
    report->pmr_after = pmr; report->group0_after = g0; report->group1_after = g1;
    if (pmr != report->pmr || g0 != report->group0 || g1 != report->group1) return 0;
    /* SRE and other entry-only CPU-interface controls reset in the following
     * CPU0 RMR return, as in the returning kernel monitor. The optional IRQ
     * interval disables delivery and drains its owned sources before this point. */
    barrier(); report->state = ready ? 3 : 4; barrier(); return ready ? 1 : 2;
}
