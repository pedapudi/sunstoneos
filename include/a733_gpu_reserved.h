#ifndef SUNSTONE_A733_GPU_RESERVED_H
#define SUNSTONE_A733_GPU_RESERVED_H
#include "a733_gpu_memory.h"
#include "a733_memory_reservations.h"
#include "a733_memory_uncached.h"
#include "pvr_gpu_service.h"

#define A733_GPU_RESERVED_WB_BYTES (34u * DEVICE_MEMORY_PAGE)
#define A733_GPU_RESERVED_NC_BYTES (45u * DEVICE_MEMORY_PAGE)

/* Private, zero-initialized once, static image storage. Serialize constructor
 * and all pool mutations before the scheduler starts. The service retains both
 * complete parent allocations for its lifetime; children cannot be released.
 * Keep source firmware and these page lists immutable. Later service faults
 * still retain allocations even when their allocator state remains DEVICE.
 * Copying or rebinding this object is forbidden; the embedded adapters do not
 * enforce protection against copies or arbitrary metadata mutation.
 */
struct a733_gpu_reserved {
    struct pvr_gpu_service_config config;
    struct a733_gpu_memory memory;
    struct pvr_mips_boot boot;
    struct pvr_mips_object mappings[PVR_FW_RESOURCE_COUNT];
    uint64_t wb_pages[34], nc_pages[45];
    struct device_memory_pool *wb_pool, *nc_pool;
    struct device_memory_lease wb_lease, nc_lease;
    struct device_memory_extent wb_extent, nc_extent;
    unsigned admitted, prepared;
};

/* Construct the fixed firmware instance for BVNC 36.56.104.183. The immutable
 * firmware must be the externally pinned build input; structural validation
 * does not authenticate it. Session and input must lie in the native identity
 * image prefix before the pools. Checks actual CPU/PA mappings across each
 * entire range and admits only the current 100 KiB code / 20 KiB data profile.
 *
 * Returns the real service configuration after native memory initialization,
 * firmware-state preparation and MIPS staging/table construction. Queue fields
 * are formatted by the existing service during OPEN. Call the existing native
 * backend bind and service init with this same configuration/memory; power,
 * observed hardware identity, interrupt ownership and independently proven
 * isolation remain mandatory. No success callback is synthesized here.
 *
 * No MMIO, cache publication, GPU startup or teardown. Failure after any
 * acquisition retains every acquired parent. Failed instances cannot retry.
 */
struct pvr_gpu_service_config *a733_gpu_reserved_prepare(struct a733_gpu_reserved *,
    uint64_t owner, const void *firmware, size_t bytes, uint32_t gpu_clock_hz);
#endif
