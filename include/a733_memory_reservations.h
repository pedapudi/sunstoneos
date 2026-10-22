#ifndef SUNSTONE_A733_MEMORY_RESERVATIONS_H
#define SUNSTONE_A733_MEMORY_RESERVATIONS_H
#include "device_memory_pool.h"
#include "a733_dma_native.h"
/* The write-back boot pool. The local layout reserves at most 2 MiB inside
 * image RAM (4 MiB with the large NPU graph) and keeps the caller's
 * alignment. The extended layout reserves class `dma32`,
 * 0x48000000..0x78000000.
 *
 * In the extended layout, a733_device_memory_prepare() runs preparation once
 * on core 0 before the scheduler starts. It checks one write translation per
 * 2 MiB block (identity, Normal write-back, inner shareable) and initializes
 * the pool; it writes no pool memory. The getter returns NULL until the
 * layout is READY.
 *
 * In the local layout, platform_init() does not yet call preparation. Until
 * it does, the first getter call prepares the pool through
 * a733_memory_prepare_once(), on whichever task or core calls first. That
 * caller must run at EL1: the check reads CurrentEL, which faults at EL0. The
 * local check makes one write translation per page. Preparation returns 1
 * when the image links no local pool, and the getter returns NULL after a
 * refusal.
 *
 * Pool mappings deny EL0 access in the extended layout. Neither layout
 * qualifies physical capacity, remote-master exclusion, device reachability
 * or bus completion. */
int a733_memory_reservations_prepare(void);
struct device_memory_pool *a733_memory_reservations_init(void);
#endif
