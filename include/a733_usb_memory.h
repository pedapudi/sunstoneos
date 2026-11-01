#ifndef SUNSTONE_A733_USB_MEMORY_H
#define SUNSTONE_A733_USB_MEMORY_H
/* The OS linker reserves this complete NC arena for memory_uncached.c.
 * Its allocation leases separate the USB parent and retained JPEG buffers.
 * Launch admission establishes external physical ownership. */
#define A733_USB_MEMORY_BASE 0x42200000
#define A733_USB_MEMORY_END 0x42a00000
#define A733_USB_MEMORY_BYTES (A733_USB_MEMORY_END-A733_USB_MEMORY_BASE)
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
_Static_assert(A733_USB_MEMORY_BASE>=0x42200000 && A733_USB_MEMORY_END<=0x43000000,
    "USB NC arena must fit between the secure monitor and radio assets");
_Static_assert(!(A733_USB_MEMORY_BASE & 0x1fffff) && !(A733_USB_MEMORY_END & 0x1fffff),
    "USB NC arena must occupy complete translation blocks");
_Static_assert(A733_USB_MEMORY_DATA_OFFSET+A733_USB_MEMORY_DATA_BYTES<=A733_USB_MEMORY_PARENT_BYTES &&
    A733_USB_MEMORY_PARENT_BYTES<=A733_USB_MEMORY_BYTES,"USB payload must fit its parent lease");
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
#endif
#endif
