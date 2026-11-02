/* SPDX-License-Identifier: Apache-2.0 */
/* Per-core interrupt counts by interrupt ID (SUNSTONE_IRQ_COUNT, off by default).
 * The wrapper counts, then calls the platform handler. Counts are written by
 * their own core only; a reader sees a relaxed snapshot. Measurement code. */
#include <stdint.h>

extern void __real_vApplicationIRQHandler(uint64_t interrupt);

#define CORES 8u
#define IDS 64u /* SGIs, PPIs and the first SPIs; the last slot also takes every larger ID */
static uint32_t counts[CORES][IDS] __attribute__((aligned(64)));

#ifdef SUNSTONE_PMU_ACCOUNT
/* Counters 0 to 2 count L1D_CACHE_REFILL (0x03), L2D_CACHE_REFILL (0x17) and STALL_BACKEND
 * (0x24) at EL0 and EL1; the cycle counter runs beside them. Enabled once per core at EL1
 * with EL0 read access (PMUSERENR EN, CR, ER); counters are never reset, no interrupt or
 * clock changes. All three events are implemented on both core types (PMCEID readback of
 * the stall-event capture). */
static unsigned pmu_done[CORES];
static void pmu_enable(uint64_t core)
{
    if (core >= CORES || pmu_done[core]) return;
    pmu_done[core] = 1;
    uint64_t pmcr;
    __asm__ volatile("mrs %0, pmcr_el0" : "=r"(pmcr));
    if (((pmcr >> 11) & 31u) < 3) return;
    const uint64_t all = 7u | (UINT64_C(1) << 31);
    __asm__ volatile("msr pmevtyper0_el0, %0\n msr pmevtyper1_el0, %1\n msr pmevtyper2_el0, %2\n msr pmccfiltr_el0, xzr\n"
                     "msr pmintenclr_el1, %3\n msr pmcntenset_el0, %3\n msr pmuserenr_el0, %4\n"
                     "msr pmcr_el0, %5\n isb"
                     :: "r"((uint64_t)0x03), "r"((uint64_t)0x17), "r"((uint64_t)0x24), "r"(all), "r"((uint64_t)0xd),
                        "r"((pmcr | 1u | (1u << 6)) & ~(uint64_t)((1u << 1) | (1u << 2) | (1u << 3))) : "memory");
}
#endif
void __wrap_vApplicationIRQHandler(uint64_t interrupt)
{
    uint64_t core;
    __asm__ volatile("mrs %0, tpidr_el1" : "=r"(core));
#ifdef SUNSTONE_PMU_ACCOUNT
    pmu_enable(core);
#endif
    if (core < CORES) {
        const unsigned slot = interrupt < IDS - 1 ? (unsigned)interrupt : IDS - 1;
        __atomic_store_n(&counts[core][slot], counts[core][slot] + 1, __ATOMIC_RELAXED);
    }
    __real_vApplicationIRQHandler(interrupt);
}
/* The name follows the FreeRTOS pattern that applications may reference. */
unsigned long ulSunstoneIrqCount(unsigned core, unsigned id)
{
    if (core >= CORES || id >= IDS) return 0;
    return __atomic_load_n(&counts[core][id], __ATOMIC_RELAXED);
}
/* The partitioned scheduler defines these (kernel/scheduler/freertos.c); the global kernel
 * has no such regions, so the application reads zero. */
__attribute__((weak)) unsigned long ulSunstoneKernelPmu(unsigned core, unsigned region, unsigned counter)
{ (void)core; (void)region; (void)counter; return 0; }
__attribute__((weak)) unsigned long ulSunstoneSgiSite(unsigned sender, unsigned index, unsigned long *site, unsigned *target)
{ (void)sender; (void)index; *site = 0; *target = 0; return 0; }
__attribute__((weak)) unsigned long ulSunstoneIdleWaiting(unsigned core) { (void)core; return 0; }
__attribute__((weak)) unsigned long ulSunstoneIdlePulls(void) { return 0; }
