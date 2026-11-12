#ifndef SUNSTONE_OS_OWNER_MEMORY_H
#define SUNSTONE_OS_OWNER_MEMORY_H
/* The memory of a device owner (the NPU runtime, the capture buffer, the headset, the USB service), taken from the unified
 * ledger when the session's grant is made (docs/designs/unified-memory.md, "Device owners"). Every size comes from the
 * application's declaration; no owner keeps a static pool. The memory is kernel-owned for the session's lifetime: no owner
 * gives it back, and the session's end (the ROM return or the reset) ends it.
 *
 * Attributes:
 *   OS_MEMORY_CACHED    Normal write-back (the default).
 *   OS_MEMORY_DEVICE    Normal non-cacheable, for rings a device writes while the CPU polls. The ledger serves it from a
 *                       non-cacheable chunk, converting a free chunk through its convert_chunk hook; an image whose ledger
 *                       has no such hook (the OS image today) refuses it.
 *   OS_MEMORY_EL0_READ  readable by the application's tasks: the application's tables map it read-only.
 *   OS_MEMORY_EL0_WRITE readable and writable by the application's tasks.
 * Without an EL0 attribute only EL1 reaches the memory: the kernel's tables map the ledger's DRAM at EL1 alone and no
 * application's tables map a kernel extent without EL0 access. An application's tables map whole 2 MiB blocks, so an
 * EL0-visible request is rounded to 2 MiB blocks on a block boundary; it must be made at admission, before the tables are
 * built (os_owner_memory refuses it later). */
#include "umem.h"
#include <stdint.h>
#include <string.h>

enum { OS_MEMORY_CACHED = 0, OS_MEMORY_DEVICE = 1u << 0, OS_MEMORY_EL0_READ = 1u << 1, OS_MEMORY_EL0_WRITE = 1u << 2 };
#define OS_MEMORY_ATTRIBUTES (OS_MEMORY_DEVICE | OS_MEMORY_EL0_READ | OS_MEMORY_EL0_WRITE)
#define OS_MEMORY_EL0_BLOCK UINT64_C(0x200000)

/* Take zeroed memory of at least bytes, aligned to align (a power of two; 0 is a page), from ledger. Returns its address, or
 * null when the request is malformed or the ledger cannot serve it. */
static inline void *os_owner_memory_from(struct umem *ledger, uint64_t bytes, uint64_t align, unsigned attributes)
{
    if (!ledger || !bytes || (attributes & ~(unsigned)OS_MEMORY_ATTRIBUTES) || (align & (align - 1)) || bytes > (UINT64_C(1) << 40))
        return 0;
    const unsigned access = attributes & OS_MEMORY_EL0_WRITE ? UMEM_PROT_READ | UMEM_PROT_WRITE :
                            attributes & OS_MEMORY_EL0_READ ? UMEM_PROT_READ : 0;
    uint64_t unit = access ? OS_MEMORY_EL0_BLOCK : UMEM_PAGE;
    if (align < unit) align = unit;
    const struct umem_request r = {.bytes = (bytes + unit - 1) & ~(unit - 1), .align = align, .owner = UMEM_OWNER_KERNEL,
                                   .flags = UMEM_RAW, .policy = attributes & OS_MEMORY_DEVICE ? UMEM_NC : UMEM_WB,
                                   .access = (uint8_t)access};
    struct umem_buf b;
    if (umem_alloc(ledger, &r, &b) != UMEM_OK) return 0;
    /* Every byte, so that no earlier contents of the extent reach an EL0 reader. */
    memset((void *)(uintptr_t)b.base, 0, (size_t)b.bytes);
    return (void *)(uintptr_t)b.base;
}
/* The same from the OS's ledger (apps/os/large.c), once admission has prepared it. */
void *os_owner_memory(uint64_t bytes, uint64_t align, unsigned attributes);
#endif
