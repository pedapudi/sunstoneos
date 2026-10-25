/* EL3 half of the SRAM-routing variant (sram-route.c): before the forced-FEL
 * reset that returns to the boot ROM, route the shared SRAM to the CPU again.
 * The ROM keeps its stack (0x000f3400) and FEL scratchpad (0x000ee800) in that
 * SRAM. EL1 has already restored the bit after local stop on the only path
 * that reaches here; this store covers that path again at the last software
 * step. A readback that still shows the NPU routing parks instead of
 * returning. The linker redirects the monitor's one call of
 * npu_finite_rom_return here. */
#include <stdint.h>

#define SRAM_ROUTING UINT32_C(0x03000008)
#define SRAM_TO_CPU UINT32_C(2)

_Noreturn void __real_npu_finite_rom_return(void);
_Noreturn void a733_smp_stop(unsigned status, uint64_t detail);

_Noreturn void __wrap_npu_finite_rom_return(void)
{
    volatile uint32_t *word = (volatile uint32_t *)(uintptr_t)SRAM_ROUTING;
    uint32_t value = *word;
    __asm__ volatile("dsb sy" ::: "memory");
    if (!(value & SRAM_TO_CPU)) {
        *word = value | SRAM_TO_CPU;
        __asm__ volatile("dsb sy" ::: "memory");
        value = *word;
        __asm__ volatile("dsb sy" ::: "memory");
    }
    if (!(value & SRAM_TO_CPU)) a733_smp_stop(7, SRAM_ROUTING);
    __real_npu_finite_rom_return();
}
