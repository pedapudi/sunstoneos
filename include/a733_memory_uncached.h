#ifndef SUNSTONE_A733_MEMORY_UNCACHED_H
#define SUNSTONE_A733_MEMORY_UNCACHED_H
#include "device_memory_pool.h"

/* Boot-owned Normal Non-cacheable reservation. The default profile uses one
 * optional 2 MiB block inside image RAM. The extended cached profile returns
 * its singleton only after bounded preparation validates both whole pools. Call only after the initial identity map is installed, before
 * any scheduler or concurrent allocation. Metadata and mappings remain private
 * and immutable for the platform lifetime. No runtime attribute transition,
 * remote-user exclusion, recovery or RAM-capacity discovery is supplied. */
struct device_memory_pool *a733_memory_uncached_init(void);
#endif
