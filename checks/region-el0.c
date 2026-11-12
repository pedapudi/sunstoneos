#include "a733_cpu_coherence.h"
/* QEMU fixture for the large application region: boots at EL1 with the MMU on, builds the kernel
 * tables of the probe platform, then runs the real ledger, space builder and EL0 routines
 * (apps/sd-read-probe/region.c, el0.S) and checks what each EL0 access did. */
#include "region.h"
#include "a733_device_memory_layout.h"
#include "umem.h"
#include "umem_cpu.h"
#include "umem_policy.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define UART ((volatile uint32_t *)0x09000000)
static void put(const char *s) { while (*s) *UART = (uint32_t)*s++; }
static void hex(uint64_t v) { char b[19] = "0x"; for (int i = 0; i < 16; ++i) b[2 + i] = "0123456789abcdef"[(v >> (60 - 4 * i)) & 15]; b[18] = 0; put(b); }
_Noreturn static void finish(int ok)
{
    put(ok ? "REGION-EL0 PASS\n" : "REGION-EL0 FAIL\n");
    static uint64_t block[2] = {0x20026, 0};
    register uint64_t op __asm__("x0") = 0x18, arg __asm__("x1") = (uint64_t)block;
    block[1] = ok ? 0 : 1;
    __asm__ volatile("hlt #0xf000" :: "r"(op), "r"(arg));
    for (;;) {}
}
static void check(int cond, const char *what, uint64_t a, uint64_t b)
{
    if (cond) return;
    put("FAIL: "); put(what); put(" "); hex(a); put(" "); hex(b); put("\n");
    finish(0);
}
void *memset(void *d, int c, size_t n) { uint8_t *p = d; while (n--) *p++ = (uint8_t)c; return d; }
void *memcpy(void *d, const void *s, size_t n) { uint8_t *p = d; const uint8_t *q = s; while (n--) *p++ = *q++; return d; }

static uint64_t level1[512] __attribute__((aligned(4096)));
static uint64_t level2[512] __attribute__((aligned(4096)));
static struct region_state state;
static struct sdp_region report;

__attribute__((used)) void exception_report(uint64_t esr, uint64_t elr, uint64_t far)
{
    put("EXCEPTION esr "); hex(esr); put(" elr "); hex(elr); put(" far "); hex(far); put("\n");
    finish(0);
}


/* ---- The unified memory ledger on the real tables ---- */
#define ATTR_COMMON (1UL | (1UL << 2) | (3UL << 8) | (1UL << 10) | (1UL << 11))
static uint8_t umem_cells[64 * 128] __attribute__((aligned(16)));
static uint8_t umem_chunks[(0x140000000UL - 0x50000000UL) / 0x200000UL];
static struct umem led;
static struct umem_el1_map el1_map;
static struct umem_policy_table kernel_table, private_tables[2];
static uint64_t split_pages[UMEM_POLICY_UPPER_SLOTS][512] __attribute__((aligned(4096)));
static uint64_t template_l1[512] __attribute__((aligned(4096)));
static uint64_t arena_storage[8][512] __attribute__((aligned(4096)));
static uint64_t a_l2[512] __attribute__((aligned(4096)));
static uint64_t kernel_l1_copy[512];
static struct app_table_arena arena;
static struct app_region_entry entries[16];
static struct app_region_ledger view;
static struct app_space space_a, space_b;

static void switch_space(uint64_t ttbr)
{
    __asm__ volatile("dsb ish\n msr ttbr0_el1,%0\n isb\n tlbi vmalle1\n dsb nsh\n isb" :: "r"(ttbr) : "memory");
}
static uint64_t attr_of(uint64_t va)                 /* PAR_EL1.ATTR of an EL1 read translation, or 0 when it faults */
{
    uint64_t par;
    __asm__ volatile("at s1e1r, %1\n isb\n mrs %0, par_el1" : "=r"(par) : "r"(va));
    return (par & 1) ? 0 : par >> 56;
}
static uint64_t run_el0(uint64_t ttbr, const char *routine, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, struct region_el0 *c)
{
    memset(c, 0, sizeof *c);
    c->entry = REGION_CODE_WINDOW + (uint64_t)(routine - region_el0_code_start);
    c->stack = REGION_STACK_WINDOW + 0x1f0000;
    c->a0 = a0; c->a1 = a1; c->a2 = a2; c->a3 = a3;
    switch_space(ttbr);
    uint64_t ec = region_el0_call(c);
    switch_space((uintptr_t)level1);
    return ec;
}
struct export_ctx { unsigned n; };
static int export_one(void *ctx, const struct umem_extent *e)
{
    struct export_ctx *x = ctx;
    entries[x->n++] = (struct app_region_entry){e->base, e->bytes, e->owner, e->access};
    return 0;
}
static void umem_phase(void)
{
    const struct umem_hooks hooks = {&el1_map, 0, 0, 0, umem_policy_convert, umem_maintain_el1};
    struct umem_range ranges[2] = {{0x50000000UL, 0x20000000UL}, {0x80000000UL, 0xc0000000UL}};
    check(umem_init(&led, &hooks, umem_cells, sizeof umem_cells / umem_cell_bytes(), umem_chunks, 0x50000000UL,
                    sizeof umem_chunks, ranges, 2) == 0, "umem init", 0, 0);
    kernel_table.l2 = level2;
    check(umem_policy_init(&el1_map, &umem_cpu_el1, level1, &kernel_table, split_pages) == 0, "policy init", 0, 0);
    memcpy(template_l1, level1, sizeof template_l1);            /* the template the space builder copies */
    memcpy(kernel_l1_copy, level1, sizeof level1);

    /* Run-time growth on the real tables: the ledger carves pages for its own entries from the memory it manages. */
    struct umem_buf small[160];
    for (unsigned i = 0; i < 160; ++i) {
        struct umem_request r = {.bytes = 4096, .owner = 3, .flags = UMEM_RAW};
        check(umem_alloc(&led, &r, &small[i]) == 0, "small alloc", i, 0);
    }
    check(led.meta_bytes >= 4096, "the ledger grew", led.meta_bytes, 0);
    struct umem_extent kext[2];
    check(umem_extents_of(&led, UMEM_OWNER_KERNEL, kext, 2) >= 1, "metadata is kernel-owned", 0, 0);
    uint64_t metadata = kext[0].base;
    for (unsigned i = 0; i < 160; ++i) check(umem_free(&led, &small[i]) == 0, "small free", i, 0);
    check(umem_check(&led) == 0, "umem check", umem_check(&led), 0);

    /* Prepare and hand-off with the real maintenance: zero by stores, clean by DC CVAC. */
    struct umem_buf wb;
    struct umem_request rw = {.bytes = 256 * 1024, .owner = 3};
    check(umem_alloc(&led, &rw, &wb) == 0, "prepared alloc", 0, 0);
    volatile uint64_t *w = (volatile uint64_t *)(uintptr_t)wb.base;
    for (uint64_t i = 0; i < wb.bytes / 8; ++i) w[i] = 0xdeadbeefdeadbeefUL;      /* the previous owner's data */
    int steps = 0, d;
    while ((d = umem_prepare_step(&led, &wb, 64 * 1024)) == 0) ++steps;
    check(d == 1 && steps >= 3, "prepare steps", d, steps);
    for (uint64_t i = 0; i < wb.bytes / 8; ++i) check(w[i] == 0, "zeroed", i, w[i]);
    check(umem_handoff(&led, &wb, UMEM_CPU, UMEM_DEVICE, 0, 0) == 0 && umem_handoff(&led, &wb, UMEM_DEVICE, UMEM_CPU, 0, 0) == 0, "handoff", 0, 0);
    check(umem_free(&led, &wb) == 0, "free", 0, 0);

    /* A slot splits into 2 MiB blocks before any space copies the table; a chunk in it then converts. */
    check(attr_of(0xc0000000UL) == 0xff, "write-back before the split", attr_of(0xc0000000UL), 0);
    check(umem_policy_split(&el1_map, 3) == 0 && attr_of(0xc0000000UL) == 0xff && attr_of(0xc0200000UL) == 0xff, "split slot 3", 0, 0);
    struct umem_buf nc;
    struct umem_request rn = {.bytes = 4096, .owner = 3, .policy = UMEM_NC, .flags = UMEM_AT, .base = 0xc0000000UL};
    check(umem_alloc(&led, &rn, &nc) == 0, "NC alloc in a split slot", 0, 0);
    while (umem_prepare_step(&led, &nc, 0) == 0) {}
    check(attr_of(0xc0000000UL) == 0x44, "NC through the table", attr_of(0xc0000000UL), 0);
    check(attr_of(0xc0200000UL) == 0xff && attr_of(0xbfe00000UL) == 0xff, "neighbors stay write-back", 0, 0);
    volatile uint64_t *n = (volatile uint64_t *)(uintptr_t)nc.base;
    n[0] = 0x1122334455667788UL; n[511] = 0x99aabbccddeeff00UL;
    check(umem_handoff(&led, &nc, UMEM_CPU, UMEM_DEVICE, 0, 0) == 0 && n[0] == 0x1122334455667788UL && n[511] == 0x99aabbccddeeff00UL, "NC contents", 0, 0);
    check(umem_handoff(&led, &nc, UMEM_DEVICE, UMEM_CPU, 0, 0) == 0 && umem_free(&led, &nc) == 0, "free", 0, 0);

    /* Two applications: 1 GiB in upper memory for one, 8 MiB in slot 1 for the other. */
    struct umem_buf big, mid;
    struct umem_request rb = {.bytes = 1UL << 30, .align = 0x200000, .owner = 1, .flags = UMEM_RAW | UMEM_AT, .base = 0x80000000UL, .access = UMEM_PROT_READ | UMEM_PROT_WRITE};
    struct umem_request rm = {.bytes = 8UL << 20, .align = 0x200000, .owner = 2, .flags = UMEM_RAW, .access = UMEM_PROT_READ | UMEM_PROT_WRITE};
    check(umem_alloc(&led, &rb, &big) == 0 && big.base == 0x80000000UL, "1 GiB placement", big.base, 0);
    check(umem_alloc(&led, &rm, &mid) == 0 && mid.base >= 0x50000000UL && mid.base < 0x70000000UL, "8 MiB placement", mid.base, 0);
    arena = (struct app_table_arena){arena_storage, (uintptr_t)arena_storage, 8, 0};
    for (unsigned who = 1; who <= 2; ++who) {
        struct export_ctx x = {0};
        memset(entries, 0, sizeof entries);
        umem_for_each(&led, (uint16_t)who, export_one, &x);
        view = (struct app_region_ledger){.entry = entries, .capacity = 16};
        struct app_space *sp = who == 1 ? &space_a : &space_b;
        check(app_space_build_l2(sp, &arena, template_l1, level2, &view, (uint16_t)who, (uint16_t)who, 0) == 0, "space build", who, 0);
    }
    /* The EL0 routines' windows, as region_prepare maps them, in each space's slot-1 table. */
    memcpy(a_l2, level2, sizeof a_l2);
    uint64_t *b_l2 = (uint64_t *)(uintptr_t)(space_b.root[1] & ~0xfffUL);
    unsigned code = (unsigned)((REGION_CODE_WINDOW - 0x40000000UL) >> 21), stack = (unsigned)((REGION_STACK_WINDOW - 0x40000000UL) >> 21);
    uint64_t *tables[2] = {a_l2, b_l2};
    for (unsigned i = 0; i < 2; ++i) {
        tables[i][code] = REGION_CODE_WINDOW | ATTR_COMMON | (3UL << 6) | (1UL << 53);
        tables[i][stack] = REGION_STACK_WINDOW | ATTR_COMMON | (1UL << 6) | (1UL << 53) | (1UL << 54);
    }
    space_a.root[1] = (uintptr_t)a_l2 | 3;
    __asm__ volatile("dsb ish" ::: "memory");
    uint64_t ttbr_a = app_space_ttbr0(&space_a), ttbr_b = app_space_ttbr0(&space_b);
    enum { OK = 0x15, ABORT = 0x24 };
    struct region_el0 c;
    uint64_t ec;
    #define EXPECT(ttbr, addr, mode, want, label) do { ec = run_el0(ttbr, region_el0_touch, addr, 8, 8, mode, &c); \
        check(ec == want, label, addr, c.esr); \
        if (want == ABORT) { check(((c.esr >> 2) & 15) == 3, label " (permission fault)", addr, c.esr); check(c.far == addr, label " (fault address)", c.far, addr); } } while (0)
    EXPECT(ttbr_a, big.base, 1, OK, "A reads and writes its own gigabyte");
    EXPECT(ttbr_a, big.base + big.bytes - 8, 1, OK, "A touches its last word");
    EXPECT(ttbr_a, big.base + big.bytes, 0, ABORT, "A beyond its region");
    EXPECT(ttbr_a, mid.base, 0, ABORT, "A reads B's buffer");
    EXPECT(ttbr_a, metadata, 0, ABORT, "A reads the ledger's metadata");
    EXPECT(ttbr_b, mid.base, 1, OK, "B reads and writes its own buffer");
    EXPECT(ttbr_b, mid.base + mid.bytes - 8, 1, OK, "B touches its last word");
    EXPECT(ttbr_b, mid.base + mid.bytes, 0, ABORT, "B beyond its buffer");
    EXPECT(ttbr_b, big.base, 0, ABORT, "B reads A's gigabyte");
    EXPECT(ttbr_b, metadata, 1, ABORT, "B writes the ledger's metadata");

    /* A buffer freed by one application is unreachable for it and reaches the next owner zeroed. */
    check(umem_free(&led, &mid) == 0, "free", 0, 0);
    check(umem_owner_of(&led, mid.base) == 0, "freed memory has no owner", 0, 0);

    /* With roots copied from the table, a 1 GiB slot can no longer split, and a chunk that needs the split is refused. */
    umem_policy_note_space(&el1_map);
    check(umem_policy_split(&el1_map, 4) == -1, "no split once a root exists", 0, 0);
    struct umem_buf refused;
    struct umem_request rr = {.bytes = 4096, .owner = 3, .policy = UMEM_NC, .flags = UMEM_AT, .base = 0x100000000UL};
    check(umem_alloc(&led, &rr, &refused) == UMEM_HW && attr_of(0x100000000UL) == 0xff, "an unsplit slot refuses the conversion", 0, 0);

    /* Slot 1 converts with the spaces' tables registered: every root sees the new attribute. */
    private_tables[0] = (struct umem_policy_table){a_l2, 0x40000000UL, 0};
    private_tables[1] = (struct umem_policy_table){b_l2, 0x40000000UL, 0};
    umem_policy_add_table(&el1_map, &private_tables[0]);
    umem_policy_add_table(&el1_map, &private_tables[1]);
    struct umem_buf nc2;
    struct umem_request r2 = {.bytes = 4096, .owner = 3, .policy = UMEM_NC, .flags = UMEM_AT, .base = 0x60000000UL};
    check(umem_alloc(&led, &r2, &nc2) == 0 && nc2.base == 0x60000000UL, "NC alloc in slot 1", nc2.base, 0);
    uint64_t roots[3] = {(uintptr_t)level1, ttbr_a, ttbr_b};
    for (unsigned i = 0; i < 3; ++i) {
        switch_space(roots[i]);
        check(attr_of(nc2.base) == 0x44, "NC in every root", i, attr_of(nc2.base));
        check(attr_of(nc2.base + 0x200000) == 0xff, "the next chunk stays write-back", i, 0);
    }
    switch_space((uintptr_t)level1);
    check(umem_check(&led) == 0, "umem check at the end", umem_check(&led), 0);
    put("umem phase ok\n");
}

void main(void)
{
    /* The tables of platform/a733/platform.c with the upper map. */
    level1[0] = (1UL << 54) | (1UL << 53) | (1UL << 10) | 1;               /* Device-nGnRnE */
    level1[1] = (uintptr_t)level2 | 3;
    for (unsigned i = 0; i < 64; ++i)
        level2[i] = (0x40000000UL + i * 0x200000UL) | (1UL << 10) | (3UL << 8) | (1UL << 2) | 1;
    for (unsigned i = 64; i < 512; ++i) level2[i] = A733_EL1_DRAM_BLOCK(0x40000000UL + i * 0x200000UL);
    for (unsigned i = 2; i < 13; ++i) level1[i] = A733_EL1_DRAM_BLOCK((uint64_t)i << 30);
    uint64_t mair = 0x44ff00, tcr = A733_CPU_COHERENCE_TCR_CACHED_WALK;
    __asm__ volatile("msr mair_el1,%0\n msr tcr_el1,%1\n msr ttbr0_el1,%2\n isb\n tlbi vmalle1\n dsb sy\n isb"
                     :: "r"(mair), "r"(tcr), "r"((uintptr_t)level1) : "memory");
    uint64_t sctlr;
    __asm__ volatile("mrs %0,sctlr_el1" : "=r"(sctlr));
    sctlr |= (1UL << 12) | (1UL << 2) | 1;
    __asm__ volatile("msr sctlr_el1,%0\n isb" :: "r"(sctlr) : "memory");
    put("MMU on\n");

    uint64_t gib = 1UL << 30;
    check(region_prepare(&state, &report, gib) == 0, "region_prepare", report.status, report.ledger_status);
    check(report.base == 0x80000000 && report.bytes == gib && report.free_after == report.free_before - gib, "region placement", report.base, report.bytes);
    check(report.tables[0] == 1 && report.tables[1] == 1 && report.tables[2] == 2, "table pages", report.tables[0], report.tables[2]);
    /* Distinct data in the first 64 MiB so the folds mean something. */
    volatile uint64_t *p = (volatile uint64_t *)(uintptr_t)report.base;
    for (uint64_t i = 0; i < (64UL << 20) / 8; ++i) p[i] = i * 0x9e3779b97f4a7c15ull + 0x1234;
    check(region_exercise(&state, &report, 96UL << 20) == 0, "region_exercise", report.stop_esr, report.stop_elr);

    /* Isolation cases, in the order region_exercise runs them. */
    enum { OK = 0x15, ABORT = 0x24 };
    struct c { unsigned ec; int fault_class; } want[] = {
        {OK, 0}, {OK, 0}, {OK, 0}, {OK, 0},          /* owner: strided read, base read, mid write, last write */
        {ABORT, 3}, {ABORT, 3},                       /* owner: first byte after, byte before the region (kernel entries: EL1 only) */
        {ABORT, 3}, {ABORT, 3}, {ABORT, 3}, {OK, 0},  /* other: read, write, read of the last word; its own stack */
        {OK, 0}, {OK, 0}, {ABORT, 3},                 /* 2 MiB-block view: read, last write, byte after */
    };
    check(report.case_count == sizeof want / sizeof want[0], "case count", report.case_count, 0);
    for (unsigned i = 0; i < report.case_count; ++i) {
        const struct sdp_region_case *k = &report.cases[i];
        check(k->ec == want[i].ec, "exception class", i, k->esr);
        if (want[i].ec == ABORT) {
            check(((k->esr >> 2) & 15) == (unsigned)want[i].fault_class, "fault class", i, k->esr);   /* 1 translation, 3 permission */
            check(k->far == k->address, "fault address", k->far, k->address);
        }
    }
    check(((report.cases[7].esr >> 6) & 1) == 1 && ((report.cases[6].esr >> 6) & 1) == 0, "write flag", report.cases[7].esr, report.cases[6].esr);
    check(report.cases[0].result == report.el1_xor_strided && report.el0_xor_owner == report.el1_xor_strided, "strided fold", report.cases[0].result, report.el1_xor_strided);
    /* The writes of the cases read back unchanged, and the page after the region was not touched. */
    check(report.flags & SDP_RT_ISOLATION, "isolation flag", report.flags, 0);

    /* Read passes: every pass completed, folds equal the EL1 fold of the same bytes. */
    check(report.pass_count == 16, "pass count", report.pass_count, 0);
    uint64_t fold40 = 0, fold96 = 0;
    for (uint64_t i = 0; i < (40UL << 20) / 8; ++i) fold40 ^= p[i];
    for (uint64_t i = 0; i < (96UL << 20) / 8; ++i) fold96 ^= p[i];
    for (unsigned i = 0; i < report.pass_count; ++i) {
        const struct sdp_region_pass *q = &report.pass[i];
        check(q->bytes == (q->kind == 0 ? 40UL << 20 : 96UL << 20), "pass bytes", i, q->bytes);
        check(q->xor_fold == (q->kind == 0 ? fold40 : fold96), "pass fold", i, q->xor_fold);
        check(q->ticks > 0, "pass time", i, q->ticks);
    }
    check(report.flags & SDP_RT_PASSES_DONE, "passes flag", report.flags, 0);
    /* The kernel table is installed again and the vectors are the caller's. */
    uint64_t ttbr, vbar;
    __asm__ volatile("mrs %0,ttbr0_el1\n mrs %1,vbar_el1" : "=r"(ttbr), "=r"(vbar));
    check(ttbr == (uintptr_t)level1 && vbar == 0, "state restored", ttbr, vbar);
    umem_phase();
    put("pmu "); hex(report.pmu_ok); put(" tlb "); hex(report.pass[3].counter[0]); put("\n");
    finish(1);
}
