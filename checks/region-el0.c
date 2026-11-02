/* QEMU fixture for the large application region: boots at EL1 with the MMU on, builds the kernel
 * tables of the probe platform, then runs the real ledger, space builder and EL0 routines
 * (apps/sd-read-probe/region.c, el0.S) and checks what each EL0 access did. */
#include "region.h"
#include "a733_device_memory_layout.h"
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

void main(void)
{
    /* The tables of platform/a733/platform.c with the upper map. */
    level1[0] = (1UL << 54) | (1UL << 53) | (1UL << 10) | 1;               /* Device-nGnRnE */
    level1[1] = (uintptr_t)level2 | 3;
    for (unsigned i = 0; i < 64; ++i)
        level2[i] = (0x40000000UL + i * 0x200000UL) | (1UL << 10) | (3UL << 8) | (1UL << 2) | 1;
    for (unsigned i = 64; i < 512; ++i) level2[i] = A733_EL1_DRAM_BLOCK(0x40000000UL + i * 0x200000UL);
    for (unsigned i = 2; i < 13; ++i) level1[i] = A733_EL1_DRAM_BLOCK((uint64_t)i << 30);
    uint64_t mair = 0x44ff00, tcr = (2UL << 32) | (1UL << 23) | (3UL << 12) | (1UL << 10) | (1UL << 8) | 25;
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
    put("pmu "); hex(report.pmu_ok); put(" tlb "); hex(report.pass[3].counter[0]); put("\n");
    finish(1);
}
