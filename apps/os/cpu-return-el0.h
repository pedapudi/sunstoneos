#ifndef SUNSTONE_OS_CPU_RETURN_EL0_H
#define SUNSTONE_OS_CPU_RETURN_EL0_H
/* The failure record of a synchronous exception from an EL0 task
 * (docs/designs/failure-return.md). apps/os/cpu-return-el0.S admits the
 * exception and calls os_cpu_return_el0_fault at EL0 with ESR_EL1, FAR_EL1 and
 * ELR_EL1. */
#include <stdint.h>
#include "sunstone_crumb.h"
/* Group application; the detail is 0xe00 plus the ESR class; bits 30:16 carry
 * ESR_EL1 bits 14:0 (for an abort, the fault status code and the write flag). */
static inline uint32_t os_cpu_return_el0_site(uint64_t esr)
{
    return SUNSTONE_CRUMB_FAIL_RETURN|(uint32_t)(esr&0x7fffu)<<16|SUNSTONE_CRUMB_FAIL_APPLICATION<<12|
        0xe00u|((uint32_t)(esr>>26)&0x3fu);
}
/* The faulting address, for an instruction abort, a PC alignment fault and a
 * data abort; zero for the other classes, where FAR_EL1 is not defined. */
static inline uint32_t os_cpu_return_el0_value(uint64_t esr,uint64_t far)
{
    const unsigned class=(unsigned)(esr>>26)&0x3fu;
    return class==0x20||class==0x22||class==0x24?(uint32_t)far:0;
}
/* The reserved stacks of apps/os/cpu-return-el0.S; absent from an image that
 * does not link it. */
extern uint8_t os_cpu_return_el0_stacks[] __attribute__((weak));
/* True at EL1 while the core's current task runs on one of them: the kernel's
 * stack overflow check compares the task's stack pointer with its own stack and
 * must not take the abandoned task for an overflow. */
static inline int os_cpu_return_el0_stack_current(void)
{
    uint64_t sp;
    __asm__ volatile("mrs %0, sp_el0":"=r"(sp));
    const uintptr_t base=(uintptr_t)os_cpu_return_el0_stacks;
    return base&&sp>base&&sp<=base+8*2048;
}
_Noreturn void os_cpu_return_el0_fault(uint64_t esr,uint64_t far,uint64_t elr);
#endif
