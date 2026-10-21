#ifndef SUNSTONE_A733_MEMORY_RESERVATIONS_H
#define SUNSTONE_A733_MEMORY_RESERVATIONS_H
#include "device_memory_pool.h"
#include "a733_dma_native.h"
/* The default profile reserves at most 2 MiB inside image RAM and validates it
 * once before concurrent users. The extended cached profile returns the same
 * boot singleton only after a733_device_memory_prepare() has validated both
 * complete linker reservations. Its mappings are privileged; EL0 clients need
 * an actual service interface. No backing writes occur during preparation.
 * All device users and remote firmware must already be excluded; mappings
 * remain pinned without aliases. Neither profile qualifies physical capacity,
 * remote exclusion, device reachability or bus completion. */
struct device_memory_pool *a733_memory_reservations_init(void);
#endif
