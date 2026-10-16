#ifndef SUNSTONE_PVR_FW_STATE_H
#define SUNSTONE_PVR_FW_STATE_H
#include "pvr_mips.h"

/* The first five IDs match enum pvr_ccb_region. */
enum pvr_fw_resource {
    PVR_FW_HOST_CONTROL, PVR_FW_HOST_COMMANDS, PVR_FW_RETURNS,
    PVR_FW_EVENT_CONTROL, PVR_FW_EVENTS, PVR_FW_POWER_SYNC,
    PVR_FW_RECOVERY_INFO, PVR_FW_MMU_SYNC, PVR_FW_SYSTEM_DATA,
    PVR_FW_FAULT_PAGE, PVR_FW_UTILIZATION, PVR_FW_RUNTIME_CONFIG,
    PVR_FW_TRACE_CONTROL, PVR_FW_TRACE_0, PVR_FW_TRACE_1,
    PVR_FW_OS_DATA, PVR_FW_CONNECTION, PVR_FW_OS_INIT, PVR_FW_SYSTEM_INIT,
    PVR_FW_RESOURCE_COUNT
};
struct pvr_fw_resource_layout { uint32_t address, bytes, allocation_bytes; };
struct pvr_fw_state_config {
    uint64_t bvnc;
    uint32_t gpu_clock_hz, clusters, slc_kilobytes;
};

/* Immutable layout for one native A733 firmware instance. Main objects occupy
 * 0xc0100000..0xc0129fff. Connection, OS-init and system-init each own a page
 * at 0xc0fd0000, 0xc0fe0000 and 0xc0ff0000. The allocator must reserve those
 * ranges, firmware code/data, and the rest of the firmware configuration area.
 * Invalid resource IDs return NULL. */
const struct pvr_fw_resource_layout *pvr_fw_state_layout(enum pvr_fw_resource resource);

/* Prepare stopped-device initialization data and mappings. The supported
 * firmware profile is BVNC 36.56.104.183, one cluster, 16 KiB SLC, ABI major 1.
 * Supply the actual configured GPU clock in Hz, not a CPU clock or peak rate.
 * Callers must validate the pinned firmware and hardware identity separately.
 *
 * Exact allocation/page counts are required. CPU byte buffers may be unaligned
 * during offline staging; typed records/page arrays require normal C alignment.
 * Every physical page must be distinct, aligned, below 2^36, and outside the
 * wrapper's intercepted ranges. The platform must prove RAM ownership, physical
 * aliases with inputs, CPU/physical correspondence, and device addressability.
 *
 * Resources 5..18 are zeroed completely and initialized. Resources 0..4 remain
 * byte-for-byte unchanged; pvr_ccb_init() owns their formatting. All mappings
 * are uncached and may be passed as objects to pvr_mips_prepare(), which checks
 * their placement against firmware code/data and page-table backing.
 *
 * Trace records contain host-only CPU pointers to the supplied trace buffers.
 * Those pointers require replacement if staged buffers move before use.
 * Inputs must remain stable and outputs exclusively owned until return.
 * Returns 1 on success; 0 leaves every buffer and mapping unchanged.
 * No allocation, cache publication, MMIO, queue activation, or live reset.
 */
int pvr_fw_state_prepare(const struct pvr_mips_buffer buffers[PVR_FW_RESOURCE_COUNT],
                         const struct pvr_fw_state_config *config,
                         struct pvr_mips_object mappings[PVR_FW_RESOURCE_COUNT]);
#endif
