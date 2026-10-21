/* SPDX-License-Identifier: Apache-2.0 */
/* PMU cycle counter and generic counter samples on the calling core, at EL1.
 * The counter registers are per core, so a caller times work on one core and
 * keeps its task there between the two samples. */
#include "a733_cpu_clock.h"
#define PMCR_E (UINT64_C(1)<<0)
#define PMCR_P (UINT64_C(1)<<1) /* Event counter reset; never written as 1. */
#define PMCR_C (UINT64_C(1)<<2) /* Cycle counter reset; never written as 1. */
#define PMCR_D (UINT64_C(1)<<3) /* Count every 64th cycle. */
#define PMCR_LC (UINT64_C(1)<<6) /* 64-bit cycle counter overflow. */
#define CYCLES (UINT64_C(1)<<31) /* PMCNTENSET_EL0.C */
static int el1(void)
{
    uint64_t el;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
    return ((el>>2)&3)==1;
}
int a733_cpu_cycles_enable(void)
{
    uint64_t pmcr,enabled=CYCLES,filter;
    if (!el1()) return 0;
    __asm__ volatile("mrs %0, pmcr_el0" : "=r"(pmcr));
    pmcr=(pmcr|PMCR_E|PMCR_LC)&~(PMCR_P|PMCR_C|PMCR_D);
    __asm__ volatile("msr pmccfiltr_el0, xzr\n msr pmcntenset_el0, %0\n msr pmcr_el0, %1\n isb\n"
        "mrs %1, pmcr_el0\n mrs %0, pmcntenset_el0\n mrs %2, pmccfiltr_el0"
        : "+r"(enabled),"+r"(pmcr),"=r"(filter) : : "memory");
    return (pmcr&(PMCR_E|PMCR_D|PMCR_LC))==(PMCR_E|PMCR_LC) && (enabled&CYCLES) && !filter;
}
int a733_cpu_cycles_sample(struct a733_cpu_cycle_sample *out)
{
    uint64_t daif,pmcr,enabled,filter;
    struct a733_cpu_cycle_sample s;
    if (!out || !el1()) return 0;
    /* IRQs stay masked between the two counter reads. */
    __asm__ volatile("mrs %0, daif\n msr daifset, #2\n isb\n mrs %1, cntpct_el0\n mrs %2, pmccntr_el0\n"
        "msr daif, %0\n mrs %3, pmcr_el0\n mrs %4, pmcntenset_el0\n mrs %5, pmccfiltr_el0\n"
        "mrs %6, midr_el1\n mrs %7, mpidr_el1"
        : "=&r"(daif),"=&r"(s.ticks),"=&r"(s.cycles),"=&r"(pmcr),"=&r"(enabled),"=&r"(filter),
          "=&r"(s.midr),"=&r"(s.mpidr) : : "memory");
    if ((pmcr&(PMCR_E|PMCR_D))!=PMCR_E || !(enabled&CYCLES) || filter) return 0;
    *out=s;return 1;
}
