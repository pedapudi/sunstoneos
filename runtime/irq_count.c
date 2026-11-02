/* SPDX-License-Identifier: Apache-2.0 */
/* Per-core interrupt counts by interrupt ID (SUNSTONE_IRQ_COUNT, off by default).
 * The wrapper counts, then calls the platform handler. Counts are written by
 * their own core only; a reader sees a relaxed snapshot. Measurement code. */
#include <stdint.h>

extern void __real_vApplicationIRQHandler(uint64_t interrupt);

#define CORES 8u
#define IDS 64u /* SGIs, PPIs and the first SPIs; the last slot also takes every larger ID */
static uint32_t counts[CORES][IDS] __attribute__((aligned(64)));

void __wrap_vApplicationIRQHandler(uint64_t interrupt)
{
    uint64_t core;
    __asm__ volatile("mrs %0, tpidr_el1" : "=r"(core));
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
