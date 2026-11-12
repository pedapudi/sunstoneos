#ifndef SUNSTONE_A733_CPU_RESET_H
#define SUNSTONE_A733_CPU_RESET_H

/* The caller publishes RVBAR and supplies a 64-bit RMR value. A warm reset
 * request takes effect at WFI; no other work or fallthrough follows the ISB. */
#define A733_CPU_RESET_WAIT(reg) dsb sy; msr rmr_el3, reg; isb; 991: wfi; b 991b

#ifndef __ASSEMBLER__
#include <stdint.h>
#define A733_RESET_STRING_INNER(...) #__VA_ARGS__
#define A733_RESET_STRING(...) A733_RESET_STRING_INNER(__VA_ARGS__)
static inline _Noreturn void a733_cpu_reset_aarch32(void)
{
    __asm__ volatile(A733_RESET_STRING(A733_CPU_RESET_WAIT(%x0))
                     :: "r"(UINT64_C(2)) : "memory");
    __builtin_unreachable();
}
#endif
#endif
