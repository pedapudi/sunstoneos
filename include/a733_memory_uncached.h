#ifndef SUNSTONE_A733_MEMORY_UNCACHED_H
#define SUNSTONE_A733_MEMORY_UNCACHED_H
#include "device_memory_pool.h"

/* The Normal non-cacheable boot pool. The local layout uses one optional
 * 2 MiB block, 0x41c00000..0x41e00000, with the caller's alignment; the
 * extended layout reserves class `uncached`, 0x78000000..0x79000000.
 *
 * Preparation and the getter behave as for the write-back pool. In the
 * extended layout a733_device_memory_prepare() runs preparation once, and in
 * the local layout a733_memory_local_prepare() does. The extended check makes
 * one write translation per 2 MiB block; the local check makes one per 2 MiB
 * block or one per page, as for the write-back pool. Both accept identity, Normal
 * non-cacheable, outer or inner shareable, and initialize the pool as
 * non-cacheable, so acquire runs no clean. Preparation returns 1 when the
 * image links no local uncached pool.
 *
 * Both EL1 and EL3 tables map the pool non-cacheable from their first
 * installation, so no cacheable alias exists. No attribute change,
 * remote-user exclusion or RAM-capacity discovery is supplied. */
int a733_memory_uncached_prepare(void);
struct device_memory_pool *a733_memory_uncached_init(void);
#endif
