#include "a733_memory_uncached.h"
#include "aarch64_device_memory.h"
#include "a733_mapping_check.h"
#include "a733_memory_critical.h"
#include "a733_device_memory_layout.h"

extern unsigned char __uncached_pool_start[], __uncached_pool_end[];
extern unsigned char __device_pool_end[];
static struct device_memory_pool uncached_pool;

#if defined(SUNSTONE_A733_EXTENDED_DEVICE_MEMORY)
int a733_memory_uncached_prepare(void)
{
    uintptr_t start = (uintptr_t)__uncached_pool_start, end = (uintptr_t)__uncached_pool_end;
    if (uncached_pool.initialized) return 1;
    /* Normal-NC identity blocks, outer or inner shareable; one write
     * translation per 2 MiB block descriptor, as for the write-back pool. */
    static const struct a733_mapping_rule nc = {A733_MAPPING_WRITE, 1,
        {{A733_MAIR_NORMAL_NC, A733_SH_OUTER | A733_SH_INNER}}, 0, 1};
    return start == A733_DEVICE_NC_START && end == A733_DEVICE_NC_END &&
        a733_mapping_check_stride(aarch64_device_translate, start, start, end - start,
                                  A733_DEVICE_MEMORY_BLOCK, &nc) &&
        device_memory_pool_init_typed(&uncached_pool, start, start, end - start, DEVICE_MEMORY_CLASS_NONE,
                                      DEVICE_MEMORY_NON_CACHEABLE);
}

struct device_memory_pool *a733_memory_uncached_init(void)
{
    return a733_device_memory_ready() ? &uncached_pool : NULL;
}
#else
static int local_result; /* See a733_memory_prepare_once(). */

static int check_local(void)
{
    uintptr_t start = (uintptr_t)__uncached_pool_start, end = (uintptr_t)__uncached_pool_end;
    if (start == end) return 1; /* The image links no uncached pool. */
    if (start != UINT32_C(0x41c00000) || end != UINT32_C(0x41e00000) ||
        (uintptr_t)__device_pool_end > start) return 0;
    /* Normal-NC identity pages, readable and writable with one shareability,
     * outer or inner; every page is checked. */
    static const struct a733_mapping_rule nc = {A733_MAPPING_READ_WRITE, 1,
        {{A733_MAIR_NORMAL_NC, A733_SH_OUTER | A733_SH_INNER}}, 1, 1};
    return a733_mapping_check_stride(aarch64_device_translate, start, start, end - start,
                                     DEVICE_MEMORY_PAGE, &nc);
}

static int init_local(void)
{
    uintptr_t start = (uintptr_t)__uncached_pool_start, end = (uintptr_t)__uncached_pool_end;
    return start == end ||
        device_memory_pool_init_typed(&uncached_pool, start, start, end - start, DEVICE_MEMORY_CLASS_NONE,
                                      DEVICE_MEMORY_NON_CACHEABLE);
}

int a733_memory_uncached_prepare(void)
{
    return a733_memory_prepare_once(&local_result, check_local, init_local);
}

struct device_memory_pool *a733_memory_uncached_init(void)
{
    /* Transitional, as for the write-back pool. */
    return a733_memory_uncached_prepare() && uncached_pool.initialized ? &uncached_pool : NULL;
}
#endif
