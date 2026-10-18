#ifndef SUNSTONE_A733_MEDIA_CLOCK_NATIVE_H
#define SUNSTONE_A733_MEDIA_CLOCK_NATIVE_H
#include <stdint.h>
/* Ordered EL1 MMIO for the media controller under its common register mutex.
 * Caller supplies authenticated Device mappings and an admitted recovery
 * contract; supervised callers retain ownership until manual power removal
 * under docs/a733-smp-supervised.md. Initial drain remains separate.
 * Parent PLL, fabric/global gates, PCK, PMIC and device registers are read-only.
 * Writes cannot assert reset, disable an enabled gate or retune a live module. */
int a733_media_clock_native_read(void *,uintptr_t,uint32_t *);
int a733_media_clock_native_write(void *,uintptr_t,uint32_t);
#endif
