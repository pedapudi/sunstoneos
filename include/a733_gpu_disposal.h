#ifndef SUNSTONE_A733_GPU_DISPOSAL_H
#define SUNSTONE_A733_GPU_DISPOSAL_H
#include "device_memory_pool.h"
#include "a733_gpu_graphics_memory.h"
struct a733_gpu_reserved;
struct a733_gpu_compute_reserved;
struct a733_gpu_graphics_memory;
struct a733_gpu_backend;
struct pvr_gpu_service;
/* Only the source-audited standalone GPU image supplies this exact static owner.
 * Global firmware/provider parents remain pinned. Seven child parent leases are
 * the maximum: three compute and four graphics. No mixed-device allocator use. */
struct a733_gpu_disposal {
    struct a733_gpu_reserved *firmware;
    struct a733_gpu_compute_reserved *compute;
    struct a733_gpu_graphics_memory *graphics;
    struct a733_gpu_backend *backend;
    uint64_t epoch, limit;
    const struct pvr_compute_package *package; /* Entire sealed package/input outside child pools. */
    struct a733_gpu_graphics_profile profile;
    uint32_t compute_owner,graphics_owner,fixed_clear,draw;
    struct pvr_ccb_token cleanup_tokens[7];
    uint32_t cleanup_status[7];
    struct device_memory_lease leases[7];
    struct device_memory_extent extents[7];
    uint32_t attributes[7];
    unsigned phase,error,count,index,page,released,transitioned;
};
enum a733_gpu_disposal_phase { A733_GPU_DISPOSAL_NEW, A733_GPU_DISPOSAL_CACHE,
    A733_GPU_DISPOSAL_UNMAP, A733_GPU_DISPOSAL_TABLES, A733_GPU_DISPOSAL_RELEASE, A733_GPU_DISPOSAL_DONE,
    A733_GPU_DISPOSAL_REBUILD, A733_GPU_DISPOSAL_LIVE, A733_GPU_DISPOSAL_FAILED };
/* Direct image binding, not an authority callback registered by a caller.
 * Other images always return NULL. Called only by the actual GPU worker. */
struct a733_gpu_disposal *gpu_application_disposal_get(struct pvr_gpu_service *);
int a733_gpu_disposal_begin(struct pvr_gpu_service *,uint64_t epoch);
int a733_gpu_disposal_poll(struct pvr_gpu_service *);
int a733_gpu_disposal_rebuild(struct pvr_gpu_service *);
#endif
