#ifndef SUNSTONE_A733_GPU_DISPOSAL_H
#define SUNSTONE_A733_GPU_DISPOSAL_H
#include "device_memory_pool.h"
#include "a733_gpu_graphics_memory.h"
struct a733_gpu_reserved;
struct a733_gpu_compute_reserved;
struct a733_gpu_graphics_memory;
struct a733_gpu_backend;
struct pvr_gpu_service;
/* Only the source-audited standalone GPU image names this exact static owner in
 * its service configuration. Global firmware/provider parents remain pinned.
 * Seven child parent leases are the maximum: three compute and four graphics.
 * No mixed-device allocator use. */
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
/* Return the configured owner only to the running service worker, while the
 * owner names a backend bound to this service and its firmware memory, has a
 * nonzero session limit, and client slot zero is the sole registered client.
 * A NULL configuration, as in every image except the standalone lifecycle
 * image, returns NULL. */
struct a733_gpu_disposal *a733_gpu_disposal_owner(struct pvr_gpu_service *);
int a733_gpu_disposal_begin(struct pvr_gpu_service *,uint64_t epoch);
int a733_gpu_disposal_poll(struct pvr_gpu_service *);
int a733_gpu_disposal_rebuild(struct pvr_gpu_service *);
#endif
