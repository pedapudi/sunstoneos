#ifndef SUNSTONE_A733_MEMORY_UNCACHED_H
#define SUNSTONE_A733_MEMORY_UNCACHED_H
#include "device_memory_pool.h"

/* The Normal non-cacheable boot pool. The local layout uses one optional
 * 2 MiB block, 0x41c00000..0x41e00000, with the caller's alignment; the
 * extended layout reserves class `uncached`, 0x78000000..0x79000000.
 *
 * Preparation runs once on core 0 before the scheduler starts, as for the
 * write-back pool. It checks one write translation per 2 MiB block (identity,
 * Normal non-cacheable, outer or inner shareable) and initializes the pool as
 * non-cacheable, so acquire runs no clean. It returns 1 when the image links
 * no local uncached pool. The getter returns NULL until preparation has
 * succeeded. Both EL1 and EL3 tables map the pool non-cacheable from their
 * first installation, so no cacheable alias exists. No attribute change,
 * remote-user exclusion or RAM-capacity discovery is supplied. */
int a733_memory_uncached_prepare(void);
struct device_memory_pool *a733_memory_uncached_init(void);
#endif
