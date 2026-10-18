#include "a733_memory_uncached.h"
#include "aarch64_device_memory.h"
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
    for (uintptr_t cpu = start; cpu < end; cpu += DEVICE_MEMORY_PAGE) {
        struct aarch64_translation r, w;
        if (!aarch64_device_translate(cpu, 0, &r) ||
            !aarch64_device_translate(cpu, 1, &w) ||
            r.physical != cpu || w.physical != cpu ||
            r.attributes != 0x44 || w.attributes != 0x44 ||
            r.shareability != w.shareability ||
            (r.shareability != 2 && r.shareability != 3)) return NULL;
    }
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
