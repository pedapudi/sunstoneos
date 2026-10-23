#ifndef SUNSTONE_A733_GPU_MAPPING_H
#define SUNSTONE_A733_GPU_MAPPING_H
#include "aarch64_device_memory.h"
#include "a733_mapping_check.h"

/* CPU mapping policy for Normal memory shared with the GPU and its firmware.
 * The page containing cpu must translate for reading to physical with MAIR
 * attribute byte attr. Non-cacheable memory (0x44) may be inner or outer
 * shareable; any other attribute must be inner shareable. write additionally
 * requires a write translation with the same address, attribute and
 * shareability. Translation cannot establish exclusive ownership or the absence
 * of aliases; the platform pins these mappings. Device-memory and other
 * policies keep their own checks. */
static inline struct a733_mapping_rule a733_gpu_mapping_rule(unsigned attr, int write)
{
    return (struct a733_mapping_rule){write ? A733_MAPPING_READ_WRITE : A733_MAPPING_READ, 1,
        {{(uint8_t)attr, attr == A733_MAIR_NORMAL_NC ? A733_SH_OUTER | A733_SH_INNER : A733_SH_INNER}}, 1, 1};
}
static inline int a733_gpu_page_mapped(uintptr_t cpu, uint64_t physical, unsigned attr, int write)
{
    const struct a733_mapping_rule rule = a733_gpu_mapping_rule(attr, write);
    return attr <= 0xffu && a733_mapping_check(aarch64_device_translate, cpu, physical, &rule);
}
/* Every page touched by a nonempty, non-wrapping identity-mapped range. */
static inline int a733_gpu_identity_mapped(uintptr_t cpu, size_t bytes, unsigned attr, int write)
{
    if (!bytes || cpu > UINTPTR_MAX - bytes || attr > 0xffu) return 0;
    const struct a733_mapping_rule rule = a733_gpu_mapping_rule(attr, write);
    return a733_mapping_check_pages(aarch64_device_translate, cpu, cpu, bytes, &rule);
}
#endif
