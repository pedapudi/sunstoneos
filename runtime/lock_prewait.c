/* SPDX-License-Identifier: Apache-2.0 */
/* Wait for the kernel task lock with the interrupt mask still open.
 *
 * Kernel entry points raise the interrupt priority mask and then queue for the
 * task lock, a ticket lock. A core that queues behind several others therefore
 * keeps its interrupts masked for the whole convoy. Each wrapper here first
 * waits, unmasked, until at most SUNSTONE_PREWAIT_DEPTH cores hold or wait for
 * the lock, then calls the real function, which masks and takes a ticket as
 * before. The wait is a hint only: it takes no ticket and no lock, so an
 * interrupt that arrives during it runs normally, and the later ticket is still
 * served in order. After SUNSTONE_PREWAIT_US microseconds the wait ends whatever
 * the lock state, so the masked wait is bounded by the ticket queue and the
 * unmasked wait by the limit; no core can be held back indefinitely.
 *
 * Only a caller that is an EL1 task with the mask fully open and interrupts
 * enabled waits. An interrupt handler, a nested critical section and an EL0 task
 * go straight to the real function with their existing mask. */
#include <stdint.h>

static inline uint64_t pw_now(void) { uint64_t t; __asm__ volatile("mrs %0, cntvct_el0" : "=r"(t)); return t; }
static inline uint32_t pw_arm(const volatile uint32_t *word)
{
    uint32_t value;
    __asm__ volatile("ldaxr %w0, [%1]" : "=&r"(value) : "r"(word) : "memory");
    return value;
}
#define SS_PW_NOW() pw_now()
#define SS_PW_ARM(word) pw_arm(word)
#define SS_PW_SLEEP() __asm__ volatile("wfe" ::: "memory")
#define SS_PW_DISARM() __asm__ volatile("clrex" ::: "memory")
#include "lock_prewait_wait.h"

#ifndef SUNSTONE_PREWAIT_DEPTH
#define SUNSTONE_PREWAIT_DEPTH 1u
#endif
#ifndef SUNSTONE_PREWAIT_US
#define SUNSTONE_PREWAIT_US 100u
#endif
#define TASK_LOCK 1u        /* index in xPortLocks; the first word of each 64-byte entry is the ticket word */
#define PMR_OPEN 0xffu
#define EL1_BIT 56u
#define CORE_SHIFT 48u
#define TCB_MASK UINT64_C(0xffffffffffff)

extern uint32_t xPortLocks[][16];
#ifdef SUNSTONE_LATENCY_SWEEP
extern void ss_latency_prewait_record(unsigned core, uint64_t ticks, unsigned timed_out);
#endif

void ss_lock_prewait(void)
{
    uint64_t task, pmr, daif;
    __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(task));
    if (!((task >> EL1_BIT) & 1u) || !(task & TCB_MASK)) return;
    __asm__ volatile("mrs %0, icc_pmr_el1\n mrs %1, daif" : "=r"(pmr), "=r"(daif));
    if (pmr != PMR_OPEN || (daif & 0x80u)) return;
    const volatile uint32_t *word = &xPortLocks[TASK_LOCK][0];
    uint32_t value = *word;
    if (ss_prewait_depth(value) <= SUNSTONE_PREWAIT_DEPTH) return;
    uint64_t frequency, start, now;
    __asm__ volatile("mrs %0, cntfrq_el0\n mrs %1, cntvct_el0" : "=r"(frequency), "=r"(start));
    /* LDAXR arms the exclusive monitor on the lock line; a ticket taken or released
     * by any core clears it and wakes the WFE. SEVL lets the first WFE fall through. */
    __asm__ volatile("sevl" ::: "memory");
    const unsigned timed_out = ss_prewait_run(word, SUNSTONE_PREWAIT_DEPTH, start, frequency / 1000000u * SUNSTONE_PREWAIT_US, &now);
#ifdef SUNSTONE_LATENCY_SWEEP
    ss_latency_prewait_record((unsigned)((task >> CORE_SHIFT) & 0xffu), now - start, timed_out);
#else
    (void)timed_out;
#endif
}

/* Tail calls keep the caller's return address, which the span recorder uses as the site. */
#define WRAP(type, name) \
    extern type __real_##name(void); \
    __attribute__((naked)) type __wrap_##name(void) \
    { \
        __asm__ volatile("stp x29, x30, [sp, #-16]!\n bl ss_lock_prewait\n ldp x29, x30, [sp], #16\n b __real_" #name); \
    }
WRAP(uint64_t, uxPortSetInterruptMask)
WRAP(void, vTaskEnterCritical)
WRAP(void, vTaskSuspendAll)
