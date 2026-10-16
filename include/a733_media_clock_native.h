#ifndef SUNSTONE_A733_MEDIA_CLOCK_NATIVE_H
#define SUNSTONE_A733_MEDIA_CLOCK_NATIVE_H
#include <stdint.h>
/* Ordered EL1 MMIO for the media controller under its common register mutex.
 * Caller must supply authenticated Device mappings and independent recovery.
 * Parent PLL, fabric/global gates, PCK, PMIC and device registers are read-only.
 * Writes cannot assert reset, disable an enabled gate or retune a live module. */
int a733_media_clock_native_read(void *,uintptr_t,uint32_t *);
int a733_media_clock_native_write(void *,uintptr_t,uint32_t);
#endif
