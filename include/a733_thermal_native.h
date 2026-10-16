#ifndef SUNSTONE_A733_THERMAL_NATIVE_H
#define SUNSTONE_A733_THERMAL_NATIVE_H
#include <stdint.h>
/* Ordered Device MMIO under the common register mutex and authenticated token.
 * Native access permits only three read-only thermal calibration SID shadows.
 * No eFuse programming/control, PMIC, GIC or PCK write is available. */
int a733_thermal_native_read(void *,uintptr_t,uint32_t *);
int a733_thermal_native_write(void *,uintptr_t,uint32_t);
#endif
