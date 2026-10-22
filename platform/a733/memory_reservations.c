#include "a733_memory_reservations.h"
#include "aarch64_device_memory.h"
#include "a733_mapping_check.h"
#include "a733_device_memory_layout.h"

extern unsigned char __device_pool_start[], __device_pool_end[], __heap_end[];
static struct device_memory_pool boot_pool;

struct device_memory_pool *a733_memory_reservations_init(void)
{
#if defined(SUNSTONE_A733_EXTENDED_DEVICE_MEMORY)
    return a733_device_memory_ready() && boot_pool.initialized ? &boot_pool : NULL;
#else
    if (boot_pool.initialized) return &boot_pool;
    uintptr_t start = (uintptr_t)__device_pool_start, end = (uintptr_t)__device_pool_end;
#if defined(SUNSTONE_NPU_LARGE_GRAPH)
    if (!a733_device_memory_local_pool_valid(start, end)) return NULL;
#endif
    if (start < UINT32_C(0x41000000) || start < (uintptr_t)__heap_end ||
        end > UINT32_C(0x41f00000) || end <= start ||
        ((start | end) & 4095) || end - start > A733_LOCAL_DEVICE_POOL_BYTES ||
        !aarch64_device_cache_stride()) return NULL;
    /* Write-back identity pages, inner shareable; the write translation is
     * checked. */
    static const struct a733_mapping_rule wb = {A733_MAPPING_WRITE, 1,
        {{A733_MAIR_NORMAL_WB, A733_SH_INNER}}, 0, 1};
    if (!a733_mapping_check_stride(aarch64_device_translate, start, start, end - start,
                                   DEVICE_MEMORY_PAGE, &wb)) return NULL;
    if (!device_memory_pool_init(&boot_pool, start, start, end - start)) return NULL;
    return &boot_pool;
#endif
}

#if defined(SUNSTONE_A733_EXTENDED_DEVICE_MEMORY)
int a733_memory_reservations_commit_verified(void)
{
    uintptr_t start = (uintptr_t)__device_pool_start, end = (uintptr_t)__device_pool_end;
    return !boot_pool.initialized && a733_device_memory_commit_allowed(0, start, end) &&
        device_memory_pool_init(&boot_pool, start, start, end - start);
}
#endif
