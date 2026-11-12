#ifndef SUNSTONE_OS_USB_MEMORY_H
#define SUNSTONE_OS_USB_MEMORY_H
#include <stdint.h>
/* Boot only, before any application root is built. Map only the controller
 * payload window read-only at EL0; all record and ring pages remain EL1-only. */
int os_usb_memory_alias(uint64_t parent_base, uint64_t parent_bytes);
#endif
