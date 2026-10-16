#ifndef SUNSTONE_A733_ROM_RETURN_H
#define SUNSTONE_A733_ROM_RETURN_H
#include <stdint.h>

/* Exclusive CPU0 early-boot use with a zero-initialized context. The caller
 * must identify the physical ROM before using its 0x20 entry. This prepares
 * registers only: retention across system reset is not yet qualified.
 * A failed operation can leave owned set; retain the context for restoration.
 * Keep a host copy before a reset that can erase SRAM and DDR state.
 */
struct a733_rom_return {
    uint32_t entry;
    uint32_t owned;
};
int a733_rom_return_prepare(struct a733_rom_return *context);
int a733_rom_return_restore(struct a733_rom_return *context);
#endif
