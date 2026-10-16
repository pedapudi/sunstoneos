#ifndef SUNSTONE_A733_MEMORY_UNCACHED_H
#define SUNSTONE_A733_MEMORY_UNCACHED_H
#include "device_memory_pool.h"

/* Optional, boot-owned Normal Non-cacheable block in the existing image RAM
 * reservation. Call only after the initial identity map is installed, before
 * any scheduler or concurrent allocation. Metadata and mappings remain private
 * and immutable for the platform lifetime. No runtime attribute transition,
 * remote-user exclusion, recovery or RAM-capacity discovery is supplied. */
struct device_memory_pool *a733_memory_uncached_init(void);
#endif
