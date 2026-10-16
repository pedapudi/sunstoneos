#ifndef SUNSTONE_A733_MEMORY_BOOT_H
#define SUNSTONE_A733_MEMORY_BOOT_H
#include <stdint.h>

extern unsigned char __uncached_pool_start[], __uncached_pool_end[], __device_pool_end[];

/* Native boot only: the image enters EL1 with MMU and data cache disabled after
 * EL3 cache invalidation. Establish one identity NC mapping before either is
 * enabled. Preserve neighboring WB mappings and require the linker's disjoint
 * complete block. Tables and mappings stay private and immutable afterward.
 * The platform separately establishes absence of remote users and aliases.
 */
static inline int a733_memory_boot_uncached(uint64_t level2[512])
{
    uintptr_t start = (uintptr_t)__uncached_pool_start;
    uintptr_t end = (uintptr_t)__uncached_pool_end;
    if (start == end) return 1;
    uint64_t sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    if ((sctlr & 5) || start != UINT32_C(0x41c00000) ||
        end != UINT32_C(0x41e00000) || (uintptr_t)__device_pool_end > start) return 0;
    unsigned index = (unsigned)((start - UINT32_C(0x40000000)) / UINT32_C(0x200000));
    uint64_t entry = level2[index];
    if ((entry & UINT64_C(0x0000ffffffe00000)) != start ||
        (entry & 31) != 5) return 0; /* Identity block, prior WB attribute 1. */
    level2[index] = (entry & ~UINT64_C(28)) | (UINT64_C(2) << 2) |
                    (UINT64_C(1) << 54) | (UINT64_C(1) << 53);
    return 1;
}
#endif
