#ifndef SUNSTONE_PVR_RENDER_TARGET_H
#define SUNSTONE_PVR_RENDER_TARGET_H
#include <stddef.h>
#include <stdint.h>

#define PVR_RENDER_COMMON_BYTES 88u
#define PVR_RENDER_DATA_BYTES 384u
#define PVR_RENDER_DATA_COUNT 2u
#define PVR_RENDER_LOCAL_PAGES 28u
/* A733 BVNC 36.56.104.183, one sample/layer, parameter format 2. Dimensions
 * cover 1..8192 pixels. Multisampling/layers require a separate real profile.
 * Sizes describe tile/parameter backing, not an RGBA output or command stream. */
struct pvr_render_profile { uint32_t width, height, global_maximum_pages; };
enum pvr_render_region { PVR_RENDER_VHEAP, PVR_RENDER_TPC,
    PVR_RENDER_MLIST0, PVR_RENDER_MLIST1, PVR_RENDER_REGION0, PVR_RENDER_REGION1,
    PVR_RENDER_REGIONS };
struct pvr_render_requirements {
    uint32_t bytes[PVR_RENDER_REGIONS], allocation[PVR_RENDER_REGIONS];
    uint32_t payload_bytes, macro_x, macro_y, tile_max_x, tile_max_y, region_headers;
};
struct pvr_render_target {
    void *common, *data[2];
    uint32_t common_firmware, data_firmware[2], local_firmware, global_firmware;
    uint64_t gpu[PVR_RENDER_REGIONS];
};
/* Pure sizing and offline record construction. Refusal leaves outputs intact.
 * Requirements/output records must be disjoint from their inputs and one
 * another. Each GPU span is a distinct, caller-owned page-aligned allocation;
 * the native adapter authenticates their RAM/VM ownership. Common/free-list records are 8-byte aligned; data records require 64-byte
 * alignment and have the exact sizes above. Build does not publish them or zero
 * parameter backing. Common/data firmware ranges must be disjoint from both
 * 64-byte free-list records, and every address must belong to the MIPS heap. */
int pvr_render_target_require(const struct pvr_render_profile *, struct pvr_render_requirements *);
int pvr_render_target_build(const struct pvr_render_profile *, const struct pvr_render_target *);
#endif
