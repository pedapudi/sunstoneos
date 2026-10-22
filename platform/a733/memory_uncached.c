#include "a733_memory_uncached.h"
#include "aarch64_device_memory.h"
#include "a733_mapping_check.h"
#if defined(SUNSTONE_A733_EXTENDED_DEVICE_MEMORY)
#include "a733_device_memory_layout.h"
#endif

extern unsigned char __uncached_pool_start[], __uncached_pool_end[];
extern unsigned char __device_pool_end[];
static struct device_memory_pool uncached_pool;

struct device_memory_pool *a733_memory_uncached_init(void)
{
#if defined(SUNSTONE_A733_EXTENDED_DEVICE_MEMORY)
    return a733_device_memory_ready() && uncached_pool.initialized ? &uncached_pool : NULL;
#else
    if (uncached_pool.initialized) return &uncached_pool;
    uintptr_t start = (uintptr_t)__uncached_pool_start;
    uintptr_t end = (uintptr_t)__uncached_pool_end;
    if (start != UINT32_C(0x41c00000) || end != UINT32_C(0x41e00000) ||
        (uintptr_t)__device_pool_end > start) return NULL;
    /* Normal-NC identity pages, readable and writable with one shareability,
     * outer or inner. */
    static const struct a733_mapping_rule nc = {A733_MAPPING_READ_WRITE, 1,
        {{A733_MAIR_NORMAL_NC, A733_SH_OUTER | A733_SH_INNER}}, 1, 1};
    if (!a733_mapping_check_stride(aarch64_device_translate, start, start, end - start,
                                   DEVICE_MEMORY_PAGE, &nc)) return NULL;
    if (!device_memory_pool_init(&uncached_pool, start, start, end - start)) return NULL;
    return &uncached_pool;
#endif
}

#if defined(SUNSTONE_A733_EXTENDED_DEVICE_MEMORY)
int a733_memory_uncached_commit_verified(void)
{
    uintptr_t start = (uintptr_t)__uncached_pool_start, end = (uintptr_t)__uncached_pool_end;
    return !uncached_pool.initialized && a733_device_memory_commit_allowed(1, start, end) &&
        device_memory_pool_init(&uncached_pool, start, start, end - start);
}
#endif
