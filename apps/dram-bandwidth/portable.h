/* SPDX-License-Identifier: GPL-2.0-only */
/* The few architecture operations of main.c, with a host fallback for the protocol check. */
#ifndef DRAM_BANDWIDTH_PORTABLE_H
#define DRAM_BANDWIDTH_PORTABLE_H
#include <stdint.h>
#ifdef __aarch64__
static inline uint64_t bw_counter(void)
{
    uint64_t v;
    __asm__ volatile("isb; mrs %0,cntpct_el0" : "=r"(v) :: "memory");
    return v;
}
static inline uint64_t bw_counter_hz(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0,cntfrq_el0" : "=r"(v));
    return v;
}
static inline void bw_relax(void) { __asm__ volatile("yield"); }
static inline void bw_publish(void) { __asm__ volatile("dsb ish" ::: "memory"); }
#else
#include <time.h>
static inline uint64_t bw_counter(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return ((uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec) * 24u / 1000u;
}
static inline uint64_t bw_counter_hz(void) { return 24000000u; }
static inline void bw_relax(void) {}
static inline void bw_publish(void) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
#endif
#endif
