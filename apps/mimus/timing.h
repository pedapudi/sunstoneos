#ifndef SUNSTONE_MIMUS_TIMING_H
#define SUNSTONE_MIMUS_TIMING_H
#ifdef SUNSTONE_MIMUS_NATIVE
#include <stdint.h>
#define COUNTER_HZ UINT64_C(24000000)
static inline uint64_t counter(void)
{
    uint64_t ticks;
    __asm__ volatile("isb\n mrs %0, cntpct_el0" : "=r"(ticks) :: "memory");
    return ticks;
}
#else
#include "boot.h"
#endif
#endif
