#include "a733_memory_uncached.h"
#include "aarch64_device_memory.h"
#include "a733_memory_blocks.h"
#include "a733_memory_critical.h"
#if defined(SUNSTONE_A733_EXTENDED_DEVICE_MEMORY)
#include "a733_device_memory_layout.h"
#endif

extern unsigned char __uncached_pool_start[], __uncached_pool_end[];
extern unsigned char __device_pool_end[];
static struct device_memory_pool uncached_pool;
#if !defined(SUNSTONE_A733_EXTENDED_DEVICE_MEMORY)
/* Local layout publication, as in memory_reservations.c. */
enum { POOL_UNPREPARED, POOL_READY, POOL_ABSENT };
static unsigned local_state;
#endif

int a733_memory_uncached_prepare(void)
{
    uintptr_t start = (uintptr_t)__uncached_pool_start;
    uintptr_t end = (uintptr_t)__uncached_pool_end;
    if (uncached_pool.initialized) return 0;
#if defined(SUNSTONE_A733_EXTENDED_DEVICE_MEMORY)
    const enum device_memory_class memory_class = DEVICE_MEMORY_CLASS_UNCACHED;
    if (start != A733_DEVICE_NC_START || end != A733_DEVICE_NC_END) return 0;
#else
    const enum device_memory_class memory_class = DEVICE_MEMORY_CLASS_NONE;
    if (start == end) { /* The image links no uncached pool. */
        __atomic_store_n(&local_state, POOL_ABSENT, __ATOMIC_RELEASE);
        return 1;
    }
    if (start != UINT32_C(0x41c00000) || end != UINT32_C(0x41e00000) ||
        (uintptr_t)__device_pool_end > start) return 0;
#endif
    /* Normal-NC identity blocks, outer or inner shareable; the write
     * translation is checked once per 2 MiB block. */
    static const struct a733_mapping_rule nc = {A733_MAPPING_WRITE, 1,
        {{A733_MAIR_NORMAL_NC, A733_SH_OUTER | A733_SH_INNER}}, 0, 1};
    if (!a733_memory_check_blocks(aarch64_device_translate, start, end, &nc) ||
        !device_memory_pool_init_typed(&uncached_pool, start, start, end - start, memory_class,
                                       DEVICE_MEMORY_NON_CACHEABLE)) return 0;
#if !defined(SUNSTONE_A733_EXTENDED_DEVICE_MEMORY)
    __atomic_store_n(&local_state, POOL_READY, __ATOMIC_RELEASE);
#endif
    return 1;
}

struct device_memory_pool *a733_memory_uncached_init(void)
{
#if defined(SUNSTONE_A733_EXTENDED_DEVICE_MEMORY)
    return a733_device_memory_ready() && uncached_pool.initialized ? &uncached_pool : NULL;
#else
    /* Transitional: until platform_init() prepares the pool before the
     * scheduler starts, a getter prepares it inside the critical section. */
    if (__atomic_load_n(&local_state, __ATOMIC_ACQUIRE) == POOL_UNPREPARED) {
        a733_memory_critical_enter();
        if (!uncached_pool.initialized) (void)a733_memory_uncached_prepare();
        a733_memory_critical_exit();
    }
    return __atomic_load_n(&local_state, __ATOMIC_ACQUIRE) == POOL_READY ? &uncached_pool : NULL;
#endif
}
