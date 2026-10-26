/* Host shim: register reads come from the model's register map. */
#ifndef SUNSTONE_A733_MMIO_H
#define SUNSTONE_A733_MMIO_H
#include <stdint.h>
uint32_t a733_mmio_read32(uintptr_t);
#endif
