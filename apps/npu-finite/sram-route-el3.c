/* EL3 half of the SRAM-routing variant (sram-route.c): before the forced-FEL
 * reset that returns to the boot ROM, route the shared SRAM to the CPU again.
 *
 * This store is the one the ROM depends on. The forced-FEL return enters the
 * ROM at 0x20, and that path zeroes 0x00044000-0x000f3ffc before it reaches
 * the bit-1 store at 0x20c; with the SRAM still routed to the NPU those CPU
 * stores would target the NPU's SRAM. Only a cold entry at 0x0 sets bit 1
 * before touching the SRAM. EL1 sets the bit after local stop too, but only
 * as an earlier diagnostic readback.
 *
 * A readback that still shows the NPU routing parks instead of returning.
 * The linker redirects the monitor's one call of npu_finite_rom_return here;
 * that function holds the image's only RMR_EL3 write. */
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
