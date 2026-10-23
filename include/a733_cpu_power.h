/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SUNSTONE_A733_CPU_POWER_H
#define SUNSTONE_A733_CPU_POWER_H
/* Idle wait for the eight-core A733 scheduler (docs/cpu-power.md).
 *
 * Every FreeRTOS idle task calls this function once per loop iteration. The
 * core stops in WFI until an interrupt is pending for it: SGI0 when another
 * core makes a task ready for it, or the timer PPI30 on core 0. An interrupt
 * that arrives before the WFI is taken first, because idle tasks run with
 * IRQs unmasked; the core then returns from the context switch to a WFI that
 * the next interrupt ends.
 *
 * DSB SY completes this core's outstanding memory accesses before it stops
 * (docs/performance-architecture.md, section 1.2). Idle tasks run at EL0, so
 * the image must set SCTLR_EL1.nTWI; otherwise WFI traps to EL1 with
 * exception class 0x01 (Arm DDI0616A.a, section E3.2).
 *
 * The image's FreeRTOSConfig.h includes this header and maps
 * vApplicationPassiveIdleHook to this function. The kernel's later extern
 * declaration of the hook then refers to this internal-linkage definition, so
 * the two instructions are inlined into both idle loops and no image needs an
 * extra source file. */
static inline void a733_cpu_idle(void)
{
    __asm__ volatile("dsb sy\n\twfi" ::: "memory");
}

#endif
