#ifndef SUNSTONE_A733_NPU_PLATFORM_NATIVE_H
#define SUNSTONE_A733_NPU_PLATFORM_NATIVE_H
#include <stdint.h>
/* Bounded, ordered EL1 MMIO callbacks. Caller holds the common register mutex
 * and authenticated platform-lifetime authority, provides Device mappings,
 * and qualifies independent watchdog recovery before any physical access.
 * Only NPU module/reset fields and PCK domain 4 policy/timings are writable.
 * Parent PLL, fabric, critical leaf gates, PCK clock and PMIC are read-only or
 * inaccessible here. Bind separate lease/clock/mutex callbacks to the core. */
int a733_npu_platform_native_read(void *, uintptr_t, uint32_t *);
int a733_npu_platform_native_write(void *, uintptr_t, uint32_t);
#endif
