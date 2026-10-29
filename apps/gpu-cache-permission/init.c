/* Runs at EL3 with MMU/caches disabled, after the monitor's cold setup.
 * Loader cache-off and monitor whole-cache invalidation exclude stale NS lines. */
#include "probe.h"
void gpu_cache_permission_initialize(void)
{
    volatile uint64_t *tags = (volatile uint64_t *)(uintptr_t)OUTPUT_BASE;
    for (unsigned i = 0; i < TAG_BYTES / 8; ++i) tags[i] = 0;
    for (unsigned i = 0; i < 3; ++i) {
        volatile uint64_t *p = (volatile uint64_t *)sample_address(i);
        for (unsigned j = 0; j < 8; ++j) p[j] = PATTERN + 8 * i + j;
    }
    __asm__ volatile("dsb sy" ::: "memory");
}
