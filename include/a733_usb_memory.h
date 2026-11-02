#ifndef SUNSTONE_A733_USB_MEMORY_H
#define SUNSTONE_A733_USB_MEMORY_H
/* The OS linker reserves this complete NC arena for memory_uncached.c.
 * Its allocation leases separate the USB parent and retained JPEG buffers.
 * Launch admission establishes external physical ownership. */
#define A733_USB_MEMORY_BASE 0x42200000
#ifndef A733_USB_MEMORY_END /* Host checks of a larger device count define it. */
#define A733_USB_MEMORY_END 0x42a00000
#endif
#define A733_USB_MEMORY_BYTES (A733_USB_MEMORY_END-A733_USB_MEMORY_BASE)
#define A733_USB_MEMORY_ALIAS_BASE 0x7a000000
#define A733_USB_MEMORY_DATA_OFFSET 0x40000
#define A733_USB_MEMORY_DATA_BYTES 0x400000
/* The parent lease is sized from the device count. Each device record owns two
 * context pages (a 64-byte input context holds 33 contexts and a device
 * context 32: 2112 and 2048 bytes) and, per endpoint, one ring page and one
 * bounce page. The controller pages (DCBAA, command and event rings, ERST,
 * scratchpad array: 16 KiB) and eight scratchpad pages come first, then
 * records fill the space before the data pool and the rest follow it.
 * SUNSTONE_USB_HOST_ENDPOINTS counts EP0 and is at most 31, the endpoint
 * contexts of an xHCI device context (xHCI 6.2.2); SUNSTONE_USB_HOST_DEVICES is
 * at most 127, the addresses of a USB bus. Other consumers of the arena (the
 * JPEG decoder's retained buffers) keep A733_USB_MEMORY_RETAINED_BYTES. */
#ifndef SUNSTONE_USB_HOST_ENDPOINTS
#define SUNSTONE_USB_HOST_ENDPOINTS 4u
#endif
#define A733_USB_MEMORY_RETAINED_BYTES 0x300000u
#define A733_USB_RECORD_BYTES (4096u*(2u+SUNSTONE_USB_HOST_ENDPOINTS*2u))
#define A733_USB_RECORD_BASE (16384u+8u*4096u)
#define A733_USB_HEAD_RECORDS ((A733_USB_MEMORY_DATA_OFFSET-A733_USB_RECORD_BASE)/A733_USB_RECORD_BYTES)
#define A733_USB_TAIL_BASE (A733_USB_MEMORY_DATA_OFFSET+A733_USB_MEMORY_DATA_BYTES)
/* Records the arena as linked holds beside the retained leases. */
#define A733_USB_RECORDS_FIT (A733_USB_HEAD_RECORDS+ \
    (A733_USB_MEMORY_BYTES-A733_USB_MEMORY_RETAINED_BYTES-A733_USB_TAIL_BASE)/A733_USB_RECORD_BYTES)
#ifndef SUNSTONE_USB_HOST_DEVICES
#define SUNSTONE_USB_HOST_DEVICES (A733_USB_RECORDS_FIT<127u?A733_USB_RECORDS_FIT:127u)
#endif
#define A733_USB_MEMORY_PARENT_FOR(n) ((n)<=A733_USB_HEAD_RECORDS?A733_USB_TAIL_BASE: \
    A733_USB_TAIL_BASE+((n)-A733_USB_HEAD_RECORDS)*A733_USB_RECORD_BYTES)
#define A733_USB_MEMORY_PARENT_BYTES A733_USB_MEMORY_PARENT_FOR(SUNSTONE_USB_HOST_DEVICES)
/* Arena the layout must reserve for n records: parent and retained leases in
 * whole 2 MiB translation blocks. For 127 records of 4 endpoints this is
 * 0xe00000 (14 MiB), against the 0x800000 now linked. */
#define A733_USB_MEMORY_ARENA_FOR(n) ((A733_USB_MEMORY_PARENT_FOR(n)+A733_USB_MEMORY_RETAINED_BYTES+0x1fffffu)&~0x1fffffu)
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
_Static_assert(SUNSTONE_USB_HOST_DEVICES>=1u&&SUNSTONE_USB_HOST_DEVICES<=127u,"USB addresses are 1 to 127");
_Static_assert(SUNSTONE_USB_HOST_ENDPOINTS>=2u&&SUNSTONE_USB_HOST_ENDPOINTS<=31u,"endpoint contexts of a device");
_Static_assert(A733_USB_MEMORY_PARENT_BYTES+A733_USB_MEMORY_RETAINED_BYTES<=A733_USB_MEMORY_BYTES,
    "the linked USB arena is too small for this device count: the layout record must reserve A733_USB_MEMORY_ARENA_FOR(devices)");
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
