/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SUNSTONE_A733_TASK_CONTEXT_H
#define SUNSTONE_A733_TASK_CONTEXT_H
/* Task-context admission predicates for the project's AArch64 FreeRTOS ports.
 *
 * Both predicates read the calling core's interrupt and critical-section
 * nesting counters, which are private to the port. With more than one core,
 * the SMP port keeps one counter pair per core, indexed by the core number
 * that it stores in TPIDR_EL1; an out-of-range index counts as nested. The
 * single-core port keeps one global pair. IRQs stay masked while the counters
 * are read, so the task cannot migrate between reading TPIDR_EL1 and indexing
 * the arrays. DAIF is restored before the scheduler is queried.
 *
 * The two predicates differ and must not be substituted for each other:
 * a733_task_context_owner_el1() requires EL1 and one designated task, while
 * a733_task_context_any_no_el_check() accepts any task and does not read
 * CurrentEL. Instance checks such as `initialized` or `self == n`, and model
 * hooks such as SUNSTONE_CAMERA_MODEL, stay at the call site. */
#include "FreeRTOS.h"
#include "task.h"
#include <stdint.h>

#if INCLUDE_xTaskGetSchedulerState != 1 || INCLUDE_xTaskGetCurrentTaskHandle != 1
#error "Task-context predicates require the scheduler-state and current-task APIs"
#endif

/* Call only with IRQs masked. */
static inline void a733_task_context_nesting_masked(uint64_t *nesting, uint64_t *critical)
{
#if configNUMBER_OF_CORES > 1
    extern volatile uint64_t ullPortInterruptNestings[], ullCriticalNestings[];
    uint64_t core;
    __asm__ volatile("mrs %0, tpidr_el1" : "=r"(core));
    *nesting = core < configNUMBER_OF_CORES ? ullPortInterruptNestings[core] : 1;
    *critical = core < configNUMBER_OF_CORES ? ullCriticalNestings[core] : 1;
#else
    extern uint64_t ullPortInterruptNesting;
    extern volatile uint64_t ullCriticalNesting;
    *nesting = ullPortInterruptNesting;
    *critical = ullCriticalNesting;
#endif
}

/* True only at EL1, with IRQs unmasked on entry, outside any interrupt handler
 * and critical section, with the scheduler running and `owner` as the current
 * task. It returns before masking IRQs when the EL or the IRQ mask already
 * refuses. A NULL owner never matches a running task. */
static inline int a733_task_context_owner_el1(TaskHandle_t owner)
{
    uint64_t el, flags, nesting, critical;
    __asm__ volatile("mrs %0, CurrentEL\n mrs %1, daif" : "=r"(el), "=r"(flags));
    if (el != 4 || (flags & 0x80)) return 0;
    __asm__ volatile("msr daifset, #2" ::: "memory");
    a733_task_context_nesting_masked(&nesting, &critical);
    __asm__ volatile("msr daif, %0" :: "r"(flags) : "memory");
    return !nesting && !critical && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING &&
        xTaskGetCurrentTaskHandle() == owner;
}

/* True with IRQs unmasked on entry, outside any interrupt handler and critical
 * section, with the scheduler running and any current task. CurrentEL is not
 * read. The counters are read before the entry IRQ mask is tested, so this
 * always masks and restores DAIF. */
static inline int a733_task_context_any_no_el_check(void)
{
    uint64_t flags, nesting, critical;
    __asm__ volatile("mrs %0, daif\n msr daifset, #2" : "=r"(flags) :: "memory");
    a733_task_context_nesting_masked(&nesting, &critical);
    __asm__ volatile("msr daif, %0" :: "r"(flags) : "memory");
    return !(flags & 0x80) && !nesting && !critical &&
        xTaskGetSchedulerState() == taskSCHEDULER_RUNNING && xTaskGetCurrentTaskHandle();
}
#endif
