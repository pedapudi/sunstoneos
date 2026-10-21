#ifndef SUNSTONE_A733_GPU_MAPPING_H
#define SUNSTONE_A733_GPU_MAPPING_H
#include "aarch64_device_memory.h"

/* CPU mapping policy for Normal memory shared with the GPU and its firmware.
 * The page containing cpu must translate for reading to physical with MAIR
 * attribute byte attr. Non-cacheable memory (0x44) may be inner or outer
 * shareable; any other attribute must be inner shareable. write additionally
 * requires a write translation with the same address, attribute and
 * shareability. Translation cannot establish exclusive ownership or the absence
 * of aliases; the platform pins these mappings. Device-memory and other
 * policies keep their own checks. */
static inline int a733_gpu_page_mapped(uintptr_t cpu, uint64_t physical, unsigned attr, int write)
{
    struct aarch64_translation r, w;
    return aarch64_device_translate(cpu, 0, &r) && r.physical == physical && r.attributes == attr &&
        (r.shareability == 3 || (attr == 0x44 && r.shareability == 2)) &&
        (!write || (aarch64_device_translate(cpu, 1, &w) && w.physical == physical &&
                    w.attributes == attr && w.shareability == r.shareability));
}
/* Every page touched by a nonempty, non-wrapping identity-mapped range. */
static inline int a733_gpu_identity_mapped(uintptr_t cpu, size_t bytes, unsigned attr, int write)
{
    if (!bytes || cpu > UINTPTR_MAX - bytes) return 0;
    uintptr_t last = (cpu + bytes - 1) & ~(uintptr_t)4095;
    for (uintptr_t page = cpu & ~(uintptr_t)4095; ; page += 4096) {
        if (!a733_gpu_page_mapped(page, page, attr, write)) return 0;
        if (page == last) return 1;
    }
}
#endif
