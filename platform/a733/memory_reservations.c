#include "a733_memory_reservations.h"
#include "aarch64_device_memory.h"
#include "a733_mapping_check.h"
#include "a733_memory_critical.h"
#include "a733_device_memory_layout.h"

extern unsigned char __device_pool_start[], __device_pool_end[], __heap_end[];
static struct device_memory_pool boot_pool;

#if defined(SUNSTONE_A733_EXTENDED_DEVICE_MEMORY)
int a733_memory_reservations_prepare(void)
{
    uintptr_t start = (uintptr_t)__device_pool_start, end = (uintptr_t)__device_pool_end;
    /* Write-back identity blocks, inner shareable. The range is whole 2 MiB
     * level-2 block descriptors, which the QEMU golden comparison of the EL1
     * tables fixes, so one write translation per block checks every page. */
    static const struct a733_mapping_rule wb = {A733_MAPPING_WRITE, 1,
        {{A733_MAIR_NORMAL_WB, A733_SH_INNER}}, 0, 1};
    return start == A733_DEVICE_WB_START && end == A733_DEVICE_WB_END &&
        a733_mapping_check_stride(aarch64_device_translate, start, start, end - start,
                                  A733_DEVICE_MEMORY_BLOCK, &wb) &&
        device_memory_pool_init_typed(&boot_pool, start, start, end - start, DEVICE_MEMORY_CLASS_DMA32,
                                      DEVICE_MEMORY_WRITE_BACK);
}

struct device_memory_pool *a733_memory_reservations_init(void)
{
    return a733_device_memory_ready() ? &boot_pool : NULL;
}
#else
static int local_result; /* See a733_memory_prepare_once(). */

static int check_local(void)
{
    uintptr_t start = (uintptr_t)__device_pool_start, end = (uintptr_t)__device_pool_end;
    if (start == end) return 1; /* The image links no pool. */
#if defined(SUNSTONE_NPU_LARGE_GRAPH)
    if (!a733_device_memory_local_pool_valid(start, end)) return 0;
#endif
    if (start < UINT32_C(0x41000000) || start < (uintptr_t)__heap_end ||
        end > UINT32_C(0x41f00000) || end < start ||
        ((start | end) & 4095) || end - start > A733_LOCAL_DEVICE_POOL_BYTES ||
        !aarch64_device_cache_stride()) return 0;
    /* Write-back identity pages, inner shareable; the write translation of
     * every page is checked, because the local tables are not compared with a
     * golden copy. */
    static const struct a733_mapping_rule wb = {A733_MAPPING_WRITE, 1,
        {{A733_MAIR_NORMAL_WB, A733_SH_INNER}}, 0, 1};
    return a733_mapping_check_stride(aarch64_device_translate, start, start, end - start,
                                     DEVICE_MEMORY_PAGE, &wb);
}

static int init_local(void)
{
    uintptr_t start = (uintptr_t)__device_pool_start, end = (uintptr_t)__device_pool_end;
    return start == end || device_memory_pool_init(&boot_pool, start, start, end - start);
}

int a733_memory_reservations_prepare(void)
{
    return a733_memory_prepare_once(&local_result, check_local, init_local);
}

struct device_memory_pool *a733_memory_reservations_init(void)
{
    /* Transitional: until platform_init() prepares the pool, the first getter
     * call prepares it. */
    return a733_memory_reservations_prepare() && boot_pool.initialized ? &boot_pool : NULL;
}
#endif
