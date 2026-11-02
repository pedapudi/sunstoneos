/* SPDX-License-Identifier: GPL-2.0-only */
/* The few architecture operations of main.c, with a host fallback for the protocol check. */
#ifndef DRAM_QOS_PORTABLE_H
#define DRAM_QOS_PORTABLE_H
#include <stdint.h>
#define DQ_NSI_BASE 0x02020000u
#define DQ_PORT(n) (DQ_NSI_BASE + 0x200u * (n))
#ifdef __aarch64__
static inline uint64_t dq_counter(void)
{ uint64_t v; __asm__ volatile("isb; mrs %0,cntpct_el0" : "=r"(v) :: "memory"); return v; }
static inline uint64_t dq_counter_hz(void)
{ uint64_t v; __asm__ volatile("mrs %0,cntfrq_el0" : "=r"(v)); return v; }
static inline void dq_relax(void) { __asm__ volatile("yield"); }
static inline uint32_t dq_reg_read(uint32_t a) { return *(volatile uint32_t *)(uintptr_t)a; }
static inline void dq_reg_write(uint32_t a, uint32_t v) { *(volatile uint32_t *)(uintptr_t)a = v; }
static inline void dq_dmb(void) { __asm__ volatile("dmb sy" ::: "memory"); }
/* Cycle counter at EL1 (MDCR_EL2.HPMN covers the counters; docs/cpu-pmu.md). */
static inline void dq_cycles_enable(void)
{
    __asm__ volatile("msr pmcntenset_el0,%0\n msr pmcr_el0,%1\n isb" :: "r"(UINT64_C(0x80000000)), "r"(UINT64_C(5)) : "memory");
}
static inline uint32_t dq_cycles(void)
{ uint64_t v; __asm__ volatile("isb\n mrs %0,pmccntr_el0" : "=r"(v) :: "memory"); return (uint32_t)v; }
/* One dependent load, timed in cycles: the load is complete (dsb ld) before the second reading. */
static inline uint32_t dq_timed_load(void **pp)
{
    uint64_t t0, t1; void *p = *pp;
    __asm__ volatile("isb\n mrs %0,pmccntr_el0\n ldr %2,[%2]\n dsb ld\n isb\n mrs %1,pmccntr_el0"
                     : "=&r"(t0), "=&r"(t1), "+r"(p) :: "memory");
    *pp = p;
    return (uint32_t)(t1 - t0);
}
#else
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
static inline uint64_t dq_counter(void)
{ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return ((uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec) * 24u / 1000u; }
static inline uint64_t dq_counter_hz(void) { return 24000000u; }
static inline void dq_relax(void) {}
static uint32_t dq_model[0x200000 / 4];
static inline int dq_known(uint32_t a)
{
    if (a < DQ_PORT(18) || a >= DQ_PORT(21)) return 0;
    const uint32_t o = (a - DQ_NSI_BASE) % 0x200;
    return o == 0xc || o == 0x10 || o == 0x14 || o == 0x18;
}
static inline uint32_t dq_reg_read(uint32_t a)
{ if (!dq_known(a)) { fprintf(stderr, "unmodeled register read %08x\n", a); exit(2); } return dq_model[(a & 0x1fffff) / 4]; }
static inline void dq_reg_write(uint32_t a, uint32_t v)
{ if (!dq_known(a)) { fprintf(stderr, "unmodeled register write %08x\n", a); exit(2); } dq_model[(a & 0x1fffff) / 4] = v; }
static inline void dq_dmb(void) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
static inline void dq_cycles_enable(void) {}
static inline uint32_t dq_cycles(void) { return (uint32_t)(dq_counter() * 80u); }
static inline uint32_t dq_timed_load(void **pp)
{ const uint32_t t0 = dq_cycles(); *pp = *(void **)*pp; return dq_cycles() - t0; }
#endif
#endif
