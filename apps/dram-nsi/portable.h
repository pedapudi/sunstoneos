/* SPDX-License-Identifier: GPL-2.0-only */
/* The few architecture operations of main.c, with a host fallback for the protocol check. */
#ifndef DRAM_NSI_PORTABLE_H
#define DRAM_NSI_PORTABLE_H
#include <stdint.h>
/* Device registers of the interconnect study. On the board the worker tasks run at EL1 and access
 * the NSI and the CCU words directly. The host model stores the registers it knows (the nine words,
 * the CCU words and the two target-agent ports) and stops on any other address. */
#define DN_NSI_BASE 0x02020000u
#define DN_TA_PORT(n) (DN_NSI_BASE + 0x200u * (n))
#ifdef __aarch64__
static inline uint32_t dn_reg_read(uint32_t a) { uint32_t v = *(volatile uint32_t *)(uintptr_t)a; return v; }
static inline void dn_reg_write(uint32_t a, uint32_t v) { *(volatile uint32_t *)(uintptr_t)a = v; }
static inline void dn_dmb(void) { __asm__ volatile("dmb sy" ::: "memory"); }
#else
#include <stdio.h>
#include <stdlib.h>
static uint32_t dn_model[0x200000 / 4];
static inline int dn_known(uint32_t a)
{
    static const uint32_t words[] = {0x0202446c,0x0202466c,0x02023a14,0x02023c14,0x02023e14,0x02024014,0x02024214,0x0202200c,0x0202220c,
                                     0x02002580,0x02002584,0x02002588};
    for (unsigned i = 0; i < sizeof words / sizeof *words; ++i) if (a == words[i]) return 1;
    for (unsigned t = 32; t < 34; ++t) {
        const uint32_t b = DN_TA_PORT(t);
        if (a == b + 0x10 || a == b + 0x14 || a == b + 0x18 || (a >= b + 0xc0 && a <= b + 0xe0 && !(a & 3))) return 1;
    }
    return 0;
}
static inline uint32_t dn_reg_read(uint32_t a)
{
    if (!dn_known(a)) { fprintf(stderr, "unmodeled register read %08x\n", a); exit(2); }
    if ((a >= DN_TA_PORT(32) + 0xcc && a <= DN_TA_PORT(32) + 0xe0) || (a >= DN_TA_PORT(33) + 0xcc && a <= DN_TA_PORT(33) + 0xe0))
        return dn_model[(a & 0x1fffff) / 4] + 1000u;
    return dn_model[(a & 0x1fffff) / 4];
}
static inline void dn_reg_write(uint32_t a, uint32_t v)
{
    if (!dn_known(a)) { fprintf(stderr, "unmodeled register write %08x\n", a); exit(2); }
    dn_model[(a & 0x1fffff) / 4] = v;
}
static inline void dn_dmb(void) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
#endif
#ifdef __aarch64__
static inline uint64_t dn_counter(void)
{
    uint64_t v;
    __asm__ volatile("isb; mrs %0,cntpct_el0" : "=r"(v) :: "memory");
    return v;
}
static inline uint64_t dn_counter_hz(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0,cntfrq_el0" : "=r"(v));
    return v;
}
static inline void dn_relax(void) { __asm__ volatile("yield"); }
static inline void dn_publish(void) { __asm__ volatile("dsb ish" ::: "memory"); }
#else
#include <time.h>
static inline uint64_t dn_counter(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return ((uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec) * 24u / 1000u;
}
static inline uint64_t dn_counter_hz(void) { return 24000000u; }
static inline void dn_relax(void) {}
static inline void dn_publish(void) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
#endif
#endif
