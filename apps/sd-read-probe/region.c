/* Large application region: spaces, EL0 isolation cases, digest cross-check and read passes. */
#include "region.h"
#include <string.h>
/* Keeps the probe's watchdog fed between passes; host fixtures define it away. */
#ifndef REGION_PROGRESS
#define REGION_PROGRESS() sunstone_probe_progress()
#endif

#define MIB (UINT64_C(1) << 20)
#define GIB (UINT64_C(1) << 30)
#define STREAM_CHUNK (64 * MIB)

static uint64_t arena_pages[16][512] __attribute__((aligned(4096)));
static uint64_t l2_copy[512] __attribute__((aligned(4096)));

static uint64_t counter_ticks(void) { uint64_t v; __asm__ volatile("isb\n mrs %0,cntvct_el0" : "=r"(v)); return v; }

static void switch_space(uint64_t ttbr)
{
    /* The EL0 windows sit in the low 128 MiB, where the kernel's entries are global; flushing at each
     * switch keeps those two blocks from meeting a stale global entry. The upper memory needs no flush
     * because every entry there carries an ASID. */
    __asm__ volatile("dsb ish\n msr ttbr0_el1,%0\n isb\n tlbi vmalle1\n dsb nsh\n isb" :: "r"(ttbr) : "memory");
}

/* The PMU: cycle counter and four event counters. */
static int pmu_start(void)
{
    /* As platform/a733/cpu_pmu.c: enable without resetting, 64-bit cycle counter. */
    uint64_t pmcr;
    __asm__ volatile("mrs %0,pmcr_el0" : "=r"(pmcr));
    if (((pmcr >> 11) & 31) < 4) return 0;
    pmcr = (pmcr | 1 | (1u << 6)) & ~(uint64_t)((1u << 1) | (1u << 2) | (1u << 3));
    __asm__ volatile("msr pmcr_el0,%0\n isb" :: "r"(pmcr));
    __asm__ volatile("msr pmevtyper0_el0,%0" :: "r"((uint64_t)0x05));   /* L1D_TLB_REFILL */
    __asm__ volatile("msr pmevtyper1_el0,%0" :: "r"((uint64_t)0x2d));   /* L2D_TLB_REFILL */
    __asm__ volatile("msr pmevtyper2_el0,%0" :: "r"((uint64_t)0x34));   /* DTLB_WALK */
    __asm__ volatile("msr pmevtyper3_el0,%0" :: "r"((uint64_t)0x03));   /* L1D_CACHE_REFILL */
    __asm__ volatile("msr pmcntenset_el0,%0\n isb" :: "r"((uint64_t)0x8000000f));
    return 1;
}
struct pmu_sample { uint64_t cycles, c[4]; };
static void pmu_read(struct pmu_sample *p)
{
    __asm__ volatile("isb\n mrs %0,pmccntr_el0\n mrs %1,pmevcntr0_el0\n mrs %2,pmevcntr1_el0\n mrs %3,pmevcntr2_el0\n mrs %4,pmevcntr3_el0"
                     : "=r"(p->cycles), "=r"(p->c[0]), "=r"(p->c[1]), "=r"(p->c[2]), "=r"(p->c[3]));
}

static void cpacr_fp(uint64_t *saved)
{
    uint64_t v;
    __asm__ volatile("mrs %0,cpacr_el1" : "=r"(v));
    *saved = v;
    __asm__ volatile("msr cpacr_el1,%0\n isb" :: "r"(v | (UINT64_C(3) << 20)));
}

static void sync_code(uint64_t address, uint64_t bytes)
{
    for (uint64_t a = address & ~63ull; a < address + bytes; a += 64) __asm__ volatile("dc cvau,%0" :: "r"(a) : "memory");
    __asm__ volatile("dsb ish" ::: "memory");
    for (uint64_t a = address & ~63ull; a < address + bytes; a += 64) __asm__ volatile("ic ivau,%0" :: "r"(a) : "memory");
    __asm__ volatile("dsb ish\n isb" ::: "memory");
}

#define ATTR_COMMON (UINT64_C(1) | (UINT64_C(1) << 2) | (UINT64_C(3) << 8) | (UINT64_C(1) << 10) | (UINT64_C(1) << 11))
int region_prepare(struct region_state *st, struct sdp_region *r, uint64_t bytes)
{
    memset(st, 0, sizeof *st);
    uint64_t ttbr, tcr;
    __asm__ volatile("mrs %0,ttbr0_el1\n mrs %1,tcr_el1" : "=r"(ttbr), "=r"(tcr));
    st->base_ttbr = ttbr;
    const uint64_t *base_l1 = (const uint64_t *)(uintptr_t)(ttbr & APP_PTE_ADDR_MASK);
    st->arena = (struct app_table_arena){arena_pages, (uintptr_t)arena_pages, 16, 0};
    /* The usable memory is what the boot tables map: 0x80000000 up to 0x3_4000_0000. */
    struct app_region_range usable = {APP_REGION_BASE, APP_REGION_LIMIT - APP_REGION_BASE};
    r->ledger_status = app_region_ledger_init(&st->ledger, &usable, 1);
    if (r->ledger_status) return r->status = 1;
    r->free_before = app_region_free_bytes(&st->ledger);
    r->ledger_status = app_region_acquire(&st->ledger, REGION_ASID_OWNER, bytes, APP_REGION_READ | APP_REGION_WRITE,
                                          &st->base, &st->bytes);
    if (r->ledger_status) return r->status = 2;
    r->base = st->base; r->bytes = st->bytes; r->free_after = app_region_free_bytes(&st->ledger);
    r->build_status[0] = app_space_build(&st->owner, &st->arena, base_l1, &st->ledger, REGION_ASID_OWNER);
    r->build_status[1] = app_space_build_view(&st->other, &st->arena, base_l1, &st->ledger, REGION_ASID_OTHER, REGION_ASID_OTHER, 0);
    r->build_status[2] = app_space_build_view(&st->view, &st->arena, base_l1, &st->ledger, REGION_ASID_OWNER, REGION_ASID_VIEW,
                                              APP_SPACE_NO_GIB_BLOCKS);
    if (r->build_status[0] || r->build_status[1] || r->build_status[2]) return r->status = 3;
    r->tables[0] = st->owner.tables; r->tables[1] = st->other.tables; r->tables[2] = st->view.tables;
    r->asid[0] = REGION_ASID_OWNER; r->asid[1] = REGION_ASID_OTHER; r->asid[2] = REGION_ASID_VIEW;
    /* EL0 windows: a read-only executable block for the routines and a read-write block for the stack,
     * in a copy of the kernel's level-2 table that every space shares. They are non-global. */
    const uint64_t *kernel_l2 = (const uint64_t *)(uintptr_t)(base_l1[1] & APP_PTE_ADDR_MASK);
    memcpy(l2_copy, kernel_l2, sizeof l2_copy);
    unsigned code = (unsigned)((REGION_CODE_WINDOW - UINT64_C(0x40000000)) >> 21), stack = (unsigned)((REGION_STACK_WINDOW - UINT64_C(0x40000000)) >> 21);
    l2_copy[code] = REGION_CODE_WINDOW | ATTR_COMMON | (UINT64_C(3) << 6) | APP_PTE_PXN;
    l2_copy[stack] = REGION_STACK_WINDOW | ATTR_COMMON | (UINT64_C(1) << 6) | APP_PTE_PXN | APP_PTE_UXN;
    __asm__ volatile("dsb ish" ::: "memory");
    struct app_space *spaces[3] = {&st->owner, &st->other, &st->view};
    for (unsigned i = 0; i < 3; ++i) spaces[i]->root[1] = (uintptr_t)l2_copy | APP_PTE_TABLE;
    /* The routines go in through the kernel's mapping of the window (EL1 read-write). */
    uint64_t size = (uint64_t)(region_el0_code_end - region_el0_code_start);
    memcpy((void *)(uintptr_t)REGION_CODE_WINDOW, region_el0_code_start, size);
    sync_code(REGION_CODE_WINDOW, size);
    st->code_window = REGION_CODE_WINDOW; st->stack_window = REGION_STACK_WINDOW;
    r->flags |= SDP_RT_PREPARED;
    return 0;
}

/* Run one EL0 routine in a space; fills a case record. */
static uint64_t run_el0(struct region_state *st, const struct app_space *space, const char *routine,
                        uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, struct region_el0 *c)
{
    memset(c, 0, sizeof *c);
    c->entry = st->code_window + (uint64_t)(routine - region_el0_code_start);
    c->stack = st->stack_window + 0x1f0000;
    c->a0 = a0; c->a1 = a1; c->a2 = a2; c->a3 = a3;
    switch_space(app_space_ttbr0(space));
    uint64_t ec = region_el0_call(c);
    switch_space(st->base_ttbr);
    return ec;
}

static void record(struct sdp_region *r, unsigned space, unsigned op, uint64_t address, uint64_t ec, const struct region_el0 *c)
{
    if (r->case_count >= SDP_RT_CASES) return;
    struct sdp_region_case *k = &r->cases[r->case_count++];
    k->space = space; k->op = op; k->ec = (uint32_t)ec; k->address = address; k->esr = c->esr; k->far = c->far; k->result = c->x0;
}

static uint64_t xor_fold(const volatile uint64_t *p, uint64_t bytes)
{
    uint64_t x = 0;
    for (uint64_t i = 0; i < bytes / 8; ++i) x ^= p[i];
    return x;
}

int region_exercise(struct region_state *st, struct sdp_region *r, uint64_t pass_cap)
{
    struct region_el0 c;
    uint64_t fpen;
    cpacr_fp(&fpen);
    uint64_t base = st->base, end = st->base + st->bytes;
    /* The EL1 view of the region's first 4 MiB, through the kernel table. */
    r->el1_xor = xor_fold((const volatile uint64_t *)(uintptr_t)base, 4 * MIB);
    uint64_t ec;
    /* Spaces: 0 owner, 1 other application, 2 owner's 2 MiB-block view. Ops: 0 read, 1 read and write back. */
    ec = run_el0(st, &st->owner, region_el0_touch, base, 4 * MIB, 4096 * 7 + 8, 0, &c);   record(r, 0, 0, base, ec, &c);
    r->el0_xor_owner = c.x0;   /* stride 28 KiB + 8: not the digest; compared below with a matching EL1 fold */
    ec = run_el0(st, &st->owner, region_el0_touch, base, 8, 8, 0, &c);                      record(r, 0, 0, base, ec, &c);
    ec = run_el0(st, &st->owner, region_el0_touch, base + st->bytes / 2, 8, 8, 1, &c);      record(r, 0, 1, base + st->bytes / 2, ec, &c);
    ec = run_el0(st, &st->owner, region_el0_touch, end - 8, 8, 8, 1, &c);                   record(r, 0, 1, end - 8, ec, &c);
    ec = run_el0(st, &st->owner, region_el0_touch, end, 8, 8, 0, &c);                       record(r, 0, 0, end, ec, &c);
    ec = run_el0(st, &st->owner, region_el0_touch, base - 8, 8, 8, 0, &c);                  record(r, 0, 0, base - 8, ec, &c);
    ec = run_el0(st, &st->other, region_el0_touch, base, 8, 8, 0, &c);                      record(r, 1, 0, base, ec, &c);
    ec = run_el0(st, &st->other, region_el0_touch, base + st->bytes / 2, 8, 8, 2, &c);      record(r, 1, 1, base + st->bytes / 2, ec, &c);
    ec = run_el0(st, &st->other, region_el0_touch, end - 8, 8, 8, 0, &c);                   record(r, 1, 0, end - 8, ec, &c);
    ec = run_el0(st, &st->other, region_el0_touch, st->stack_window, 8, 8, 1, &c);          record(r, 1, 2, st->stack_window, ec, &c);   /* control: its own stack window */
    ec = run_el0(st, &st->view, region_el0_touch, base, 8, 8, 0, &c);                       record(r, 2, 0, base, ec, &c);
    ec = run_el0(st, &st->view, region_el0_touch, end - 8, 8, 8, 1, &c);                    record(r, 2, 1, end - 8, ec, &c);
    ec = run_el0(st, &st->view, region_el0_touch, end, 8, 8, 0, &c);                        record(r, 2, 0, end, ec, &c);
    /* A fold of the first 4 MiB with the same stride at EL1 for the comparison above. */
    uint64_t x = 0;
    for (uint64_t a = base; a < base + 4 * MIB; a += 4096 * 7 + 8) x ^= *(volatile uint64_t *)(uintptr_t)a;
    r->el1_xor_strided = x;
    r->flags |= SDP_RT_ISOLATION;

    /* Read passes. */
    r->pmu_ok = pmu_start();
    for (unsigned kind = 0; kind < 3; ++kind) {
        const struct app_space *space = kind == 2 ? &st->view : &st->owner;
        unsigned passes = kind == 0 ? 10 : 3;
        uint64_t bytes = kind == 0 ? 40 * MIB : (st->bytes < GIB ? st->bytes : GIB);
        if (pass_cap && bytes > pass_cap) bytes = pass_cap;
        for (unsigned i = 0; i < passes && r->pass_count < SDP_RT_PASSES; ++i) {
            struct pmu_sample p0, p1;
            uint64_t xorv = 0, t0, t1;
            switch_space(app_space_ttbr0(space));
            pmu_read(&p0);
            t0 = counter_ticks();
            for (uint64_t off = 0; off < bytes; off += STREAM_CHUNK) {
                uint64_t n = bytes - off < STREAM_CHUNK ? bytes - off : STREAM_CHUNK;
                memset(&c, 0, sizeof c);
                c.entry = st->code_window + (uint64_t)(region_el0_stream - region_el0_code_start);
                c.stack = st->stack_window + 0x1f0000;
                c.a0 = base + off; c.a1 = n;
                uint64_t e = region_el0_call(&c);
                if (e != 0x15) { r->status = 10; switch_space(st->base_ttbr); return r->status; }
                xorv ^= c.x0;
            }
            t1 = counter_ticks();
            pmu_read(&p1);
            switch_space(st->base_ttbr);
            REGION_PROGRESS();
            struct sdp_region_pass *o = &r->pass[r->pass_count++];
            o->kind = kind; o->bytes = bytes; o->ticks = t1 - t0; o->cycles = p1.cycles - p0.cycles;
            for (unsigned k = 0; k < 4; ++k) o->counter[k] = p1.c[k] - p0.c[k];
            o->xor_fold = xorv;
        }
    }
    __asm__ volatile("msr cpacr_el1,%0\n isb" :: "r"(fpen));
    r->flags |= SDP_RT_PASSES_DONE;
    return 0;
}
