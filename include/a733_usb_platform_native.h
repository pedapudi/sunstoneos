#ifndef SUNSTONE_A733_USB_PLATFORM_NATIVE_H
#define SUNSTONE_A733_USB_PLATFORM_NATIVE_H
#include <stdint.h>
/* Ordered native MMIO under authenticated Device mappings and the common
 * register mutex. Fixed USB0 device profile: no PMIC, fabric, host, DMA,
 * watchdog, power-controller or soft-connect writes are accepted. */
int a733_usb_platform_native_read(void *,uintptr_t,uint32_t *);
int a733_usb_platform_native_write(void *,uintptr_t,uint32_t);
#endif
