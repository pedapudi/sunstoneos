#include "a733_memory_reservations.h"
#include "aarch64_device_memory.h"
#include "a733_memory_blocks.h"
#include "a733_memory_critical.h"
#include "a733_device_memory_layout.h"

extern unsigned char __device_pool_start[], __device_pool_end[], __heap_end[];
static struct device_memory_pool boot_pool;
#if !defined(SUNSTONE_A733_EXTENDED_DEVICE_MEMORY)
/* Local layout: 0 until preparation, then POOL_READY or POOL_ABSENT. Written
 * with release order inside the critical section and read with acquire order,
 * because a getter can still prepare the pool. */
enum { POOL_UNPREPARED, POOL_READY, POOL_ABSENT };
static unsigned local_state;
#endif

int a733_memory_reservations_prepare(void)
{
    uintptr_t start = (uintptr_t)__device_pool_start, end = (uintptr_t)__device_pool_end;
    if (boot_pool.initialized) return 0;
#if defined(SUNSTONE_A733_EXTENDED_DEVICE_MEMORY)
    const enum device_memory_class memory_class = DEVICE_MEMORY_CLASS_DMA32;
    if (start != A733_DEVICE_WB_START || end != A733_DEVICE_WB_END) return 0;
#else
    const enum device_memory_class memory_class = DEVICE_MEMORY_CLASS_NONE;
    if (start == end) { /* The image links no pool. */
        __atomic_store_n(&local_state, POOL_ABSENT, __ATOMIC_RELEASE);
        return 1;
    }
#if defined(SUNSTONE_NPU_LARGE_GRAPH)
    if (!a733_device_memory_local_pool_valid(start, end)) return 0;
#endif
    if (start < UINT32_C(0x41000000) || start < (uintptr_t)__heap_end ||
        end > UINT32_C(0x41f00000) || end < start ||
        ((start | end) & 4095) || end - start > A733_LOCAL_DEVICE_POOL_BYTES) return 0;
#endif
    /* Write-back identity blocks, inner shareable; the write translation is
     * checked once per 2 MiB block. */
    static const struct a733_mapping_rule wb = {A733_MAPPING_WRITE, 1,
        {{A733_MAIR_NORMAL_WB, A733_SH_INNER}}, 0, 1};
    if (!aarch64_device_cache_stride() ||
        !a733_memory_check_blocks(aarch64_device_translate, start, end, &wb)) return 0;
    if (!device_memory_pool_init_typed(&boot_pool, start, start, end - start, memory_class,
                                       DEVICE_MEMORY_WRITE_BACK)) return 0;
#if !defined(SUNSTONE_A733_EXTENDED_DEVICE_MEMORY)
    __atomic_store_n(&local_state, POOL_READY, __ATOMIC_RELEASE);
#endif
    return 1;
}

struct device_memory_pool *a733_memory_reservations_init(void)
{
#if defined(SUNSTONE_A733_EXTENDED_DEVICE_MEMORY)
    return a733_device_memory_ready() && boot_pool.initialized ? &boot_pool : NULL;
#else
    /* Transitional: until platform_init() prepares the pool before the
     * scheduler starts, a getter prepares it inside the critical section. */
    if (__atomic_load_n(&local_state, __ATOMIC_ACQUIRE) == POOL_UNPREPARED) {
        a733_memory_critical_enter();
        if (!boot_pool.initialized) (void)a733_memory_reservations_prepare();
        a733_memory_critical_exit();
    }
    return __atomic_load_n(&local_state, __ATOMIC_ACQUIRE) == POOL_READY ? &boot_pool : NULL;
#endif
}
