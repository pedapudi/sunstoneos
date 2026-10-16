#ifndef SUNSTONE_A733_DMA_PLATFORM_NATIVE_H
#define SUNSTONE_A733_DMA_PLATFORM_NATIVE_H
#include <stdint.h>
/* Ordered EL1 MMIO access only. Caller provides Device mappings, qualified
 * independent watchdog recovery, exclusive authenticated handoff, stable parent
 * authority and the common register mutex. No GPIO, PMIC or persistent writes.
 * Parent/fabric and DMA channel registers are read-only in this adapter. */
int a733_dma_platform_native_read(void *, uintptr_t, uint32_t *);
int a733_dma_platform_native_write(void *, uintptr_t, uint32_t);
#endif
