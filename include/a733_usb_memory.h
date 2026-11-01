#ifndef SUNSTONE_A733_USB_MEMORY_H
#define SUNSTONE_A733_USB_MEMORY_H
/* Fixed USB class memory profile. Physical ownership is admitted separately
 * from these geometry constants by the sealed OS session before MMU setup. */
#define A733_USB_MEMORY_BASE 0x47000000
#define A733_USB_MEMORY_END 0x48000000
#define A733_USB_MEMORY_ALIAS_BASE 0x7a000000
#define A733_USB_MEMORY_PARENT_BYTES 0x500000
#define A733_USB_MEMORY_DATA_OFFSET 0x40000
#define A733_USB_MEMORY_DATA_BYTES 0x400000
#define A733_USB_MEMORY_ALIAS_TABLES 3
#ifndef __ASSEMBLER__
#include <stddef.h>
#include <stdint.h>
#define A733_USB_MEMORY_DATA_START ((uintptr_t)A733_USB_MEMORY_BASE+A733_USB_MEMORY_DATA_OFFSET)
#define A733_USB_MEMORY_DATA_END (A733_USB_MEMORY_DATA_START+A733_USB_MEMORY_DATA_BYTES)
#define A733_USB_MEMORY_APP_START ((uintptr_t)A733_USB_MEMORY_ALIAS_BASE+A733_USB_MEMORY_DATA_OFFSET)
#define A733_USB_MEMORY_APP_END (A733_USB_MEMORY_APP_START+A733_USB_MEMORY_DATA_BYTES)
/* Zero-length and crossing ranges have no application view. This conversion
 * does not grant a lease or authorize access to a device-owned buffer. */
static inline const void *a733_usb_memory_to_app(const void *pointer,size_t bytes)
{
    uintptr_t p=(uintptr_t)pointer;
    return bytes&&p>=A733_USB_MEMORY_DATA_START&&p<A733_USB_MEMORY_DATA_END&&
        bytes<=A733_USB_MEMORY_DATA_END-p?
        (const void *)(p-A733_USB_MEMORY_BASE+A733_USB_MEMORY_ALIAS_BASE):NULL;
}
static inline void *a733_usb_memory_from_app(const void *pointer,size_t bytes)
{
    uintptr_t p=(uintptr_t)pointer;
    return bytes&&p>=A733_USB_MEMORY_APP_START&&p<A733_USB_MEMORY_APP_END&&
        bytes<=A733_USB_MEMORY_APP_END-p?
        (void *)(p-A733_USB_MEMORY_ALIAS_BASE+A733_USB_MEMORY_BASE):NULL;
}
int os_usb_memory_admitted(void);
#endif
#endif
