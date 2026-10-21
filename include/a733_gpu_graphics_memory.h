#ifndef SUNSTONE_A733_GPU_GRAPHICS_MEMORY_H
#define SUNSTONE_A733_GPU_GRAPHICS_MEMORY_H
#include "a733_gpu_compute_reserved.h"
#include "pvr_render_target.h"
#include "pvr_render_service.h"

/* Maximum depth/texture profile: root + three directories + 23 leaf tables.
 * Every constructor still derives and checks the complete actual VM plan. */
#define A733_GPU_GRAPHICS_TABLE_PAGES 27u
#define A733_GPU_GRAPHICS_MAPS (PVR_RENDER_REGIONS + 2u)
#define A733_GPU_GRAPHICS_MAX_MAPS (A733_GPU_GRAPHICS_MAPS + 8u)
#define A733_GPU_RENDER_FW UINT32_C(0xc0a00000)
enum a733_gpu_graphics_phase { A733_GPU_GRAPHICS_NEW, A733_GPU_GRAPHICS_ACQUIRED,
    A733_GPU_GRAPHICS_BOUND, A733_GPU_GRAPHICS_DATA, A733_GPU_GRAPHICS_TABLES,
    A733_GPU_GRAPHICS_READY, A733_GPU_GRAPHICS_FAILED, A733_GPU_GRAPHICS_DISPOSED };
struct a733_gpu_graphics_profile {
    struct pvr_render_profile render;
    uint32_t initial_pages, grow_pages, threshold;
    uint64_t operation_timeout_us;
};
/* One static private owner, zero once. Inputs are copied before allocation.
 * The four complete parents remain held for the boot. Child leases are finite
 * page indices inside the backing parent, never independent pool allocations.
 * The manager exclusively owns pages[]; every other descriptor stays immutable.
 * No copying, rebinding, release or retry of an admitted instance is supported.
 */
struct a733_gpu_graphics_memory {
    uintptr_t self;
    struct a733_gpu_reserved *firmware;
    struct a733_gpu_compute_reserved *compute;
    struct pvr_gpu_service *service;
    struct a733_gpu_graphics_profile profile;
    uint64_t owner, epoch;
    struct device_memory_lease backing_lease, payload_lease, shared_lease, table_lease;
    struct device_memory_extent backing_extent, payload_extent, shared_extent, table_extent;
    struct pvr_render_requirements render;
    struct pvr_render_target target;
    struct pvr_gpu_mapping maps[A733_GPU_GRAPHICS_MAX_MAPS];
    struct pvr_gpu_vm_requirements requirements;
    struct a733_gpu_vm_memory vm;
    uint64_t table_pages[A733_GPU_GRAPHICS_TABLE_PAGES], firmware_page;
    struct pvr_mips_object object, render_object;
    uint64_t render_pages[3];
    struct pvr_render_service submission;
    struct pvr_free_list_config lists[2]; /* local, global */
    struct pvr_free_list_page local_pages[PVR_RENDER_LOCAL_PAGES];
    struct pvr_free_list_page global_pages[PVR_FREE_LIST_MAX_PAGES];
    uint32_t acquired[2], cursor, fixed_clear, draw;
    enum a733_gpu_graphics_phase phase;
};
/* Native identity-image constructor, before scheduler/backend binding. Require
 * the actual reserved firmware and bound fixed compute owner sharing one
 * uninitialized service. Authenticated WB/NC pools admit complete parent RAM;
 * capacity refusal never reduces the requested profile. Maximum global backing
 * is currently 64 MiB, a manager limit, not the final graphics capacity target.
 * On success attach concrete free-list ops and the typed graphics owner to the
 * same service config, build protected table mappings and firmware HWRT bytes.
 * No power/MMIO/publication. The caller subsequently binds the real backend.
 */
int a733_gpu_graphics_memory_prepare(struct a733_gpu_graphics_memory *,
    struct a733_gpu_reserved *, struct a733_gpu_compute_reserved *,
    struct pvr_gpu_service *, uint64_t owner, const struct a733_gpu_graphics_profile *);
/* Same native construction with real fixed 64x64 clear resources. The supplied
 * render dimensions must be 64x64. Existing prepare remains resource-only. */
int a733_gpu_graphics_memory_prepare_clear(struct a733_gpu_graphics_memory *,
    struct a733_gpu_reserved *, struct a733_gpu_compute_reserved *,
    struct pvr_gpu_service *, uint64_t owner, const struct a733_gpu_graphics_profile *);
/* Same render ownership with one bounded copied triangle-list submission.
 * Adds one GPU-read-only vertex page; existing clear construction is unchanged. */
int a733_gpu_graphics_memory_prepare_draw(struct a733_gpu_graphics_memory *,
    struct a733_gpu_reserved *,struct a733_gpu_compute_reserved *,
    struct pvr_gpu_service *,uint64_t owner,const struct a733_gpu_graphics_profile *);
int a733_gpu_graphics_memory_prepare_texture(struct a733_gpu_graphics_memory *,
    struct a733_gpu_reserved *,struct a733_gpu_compute_reserved *,
    struct pvr_gpu_service *,uint64_t owner,const struct a733_gpu_graphics_profile *);
int a733_gpu_graphics_memory_prepare_depth_blend(struct a733_gpu_graphics_memory *,
    struct a733_gpu_reserved *,struct a733_gpu_compute_reserved *,
    struct pvr_gpu_service *,uint64_t owner,const struct a733_gpu_graphics_profile *);
/* Called before service preparation writes. Validates the combined native
 * parents, metadata/config spans, exact descriptors and compute ownership. */
int a733_gpu_graphics_memory_validate(struct a733_gpu_graphics_memory *,
    const struct pvr_gpu_service_config *, const struct pvr_gpu_service *);
/* Worker-only, after initial lists finish, before power: clean one payload page
 * or publish one VM table page per poll. Return 1 completed, 0 pending, -1 fault.
 * Service observes its shared absolute OPEN deadline before/after each call.
 */
int a733_gpu_graphics_memory_begin(struct a733_gpu_graphics_memory *);
int a733_gpu_graphics_memory_poll(struct a733_gpu_graphics_memory *);
/* Terminal memory-only latch. Parents/child leases stay held. Later calls must
 * perform no translation/cache/shared-memory access. Reclamation needs real
 * device isolation, firmware drain and explicit disposal, none supplied here. */
void a733_gpu_graphics_memory_abort(struct a733_gpu_graphics_memory *);
/* Direct native identity check for the embedded render service. No callback
 * substitution can authenticate a different allocator or graphics owner. */
int a733_gpu_graphics_memory_render_admitted(struct a733_gpu_graphics_memory *);
/* Snapshot only the actual admitted owner's firmware object addresses.
 * Ordered fragment/geometry (if present), HWRT[1/0], local/global lists. */
int a733_gpu_graphics_memory_cleanup_records(struct a733_gpu_graphics_memory *,
    uint32_t addresses[6],uint32_t types[6],unsigned *count);
uintptr_t a733_gpu_graphics_memory_render_shared(const struct a733_gpu_graphics_memory *);
int a733_gpu_graphics_memory_disposed(struct a733_gpu_graphics_memory *);
int a733_gpu_graphics_memory_rebuild(struct a733_gpu_graphics_memory *,struct pvr_gpu_service *);
#endif
