#ifndef SUNSTONE_A733_GPU_MIMUS_H
#define SUNSTONE_A733_GPU_MIMUS_H
/* Native construction of the mimus GPU session (docs/gpu-mimus-service.md):
 * pool pages, the GPU address space, the compute context and its
 * context-switch programs, bound to the GPU service as its mimus component.
 * The A733 counterpart of a733_gpu_compute_reserved for the fixed session. */
#include "a733_gpu_reserved.h"
#include "pvr_mimus_service.h"
#include "pvr_compute_programs.h"

/* Write-back pages: the job page, two program pages and the context-switch
 * programs (1 + 1 + 4 x 1 + 1 + 5 + 5 + 1 pages). */
#define A733_GPU_MIMUS_PAYLOAD_PAGES 21u
/* GPU mappings: base model, overlay model, arena span and output region as
 * 2 MiB pages, then one 4 KiB mapping per payload page. */
#define A733_GPU_MIMUS_MAPS (4u + A733_GPU_MIMUS_PAYLOAD_PAGES)
/* The planner's table pages for those mappings: 24, measured by
 * checks/pvr-mimus-service.py; with the payload and the firmware's 34 pages
 * the GPU holds 79 of the write-back pool's pages. */
#define A733_GPU_MIMUS_TABLE_PAGES 24u

struct a733_gpu_mimus_layout {
    /* Physical addresses of the two model files, each in one 8 MiB region,
     * of the arena span (12 MiB) and of the output region (2 MiB). The arena
     * and the output region are mapped at their own addresses. */
    uint64_t models[PVR_MIMUS_MODELS], arena, output;
    /* Indexed by ordinal; every entry already admitted. */
    const struct pvr_mimus_tensor *tensors;
    uint32_t tensor_count;
    uint64_t max_job_us;
};
/* Private static image storage, zeroed once. */
struct a733_gpu_mimus {
    struct device_memory_lease payload_lease, firmware_lease, table_lease;
    struct device_memory_extent payload_extent, firmware_extent, table_extent;
    struct pvr_mimus_service component;
    struct pvr_mimus_service_config config;
    struct pvr_compute_program_config programs;
    struct pvr_compute_program_result program_result;
    struct a733_gpu_vm_memory vm;
    struct pvr_gpu_vm_requirements requirements;
    struct pvr_gpu_mapping maps[A733_GPU_MIMUS_MAPS];
    uint64_t payload_pages[A733_GPU_MIMUS_PAYLOAD_PAGES];
    uint64_t firmware_pages[PVR_MIMUS_SERVICE_OBJECTS];
    uint64_t table_pages[A733_GPU_MIMUS_TABLE_PAGES];
    unsigned stage;
};
/* Before the scheduler, at EL1, after a733_gpu_reserved_prepare and before
 * a733_gpu_backend_bind and pvr_gpu_service_init. Acquires the payload and
 * table pages from the firmware's write-back pool and the six firmware
 * objects from its uncached pool, builds the address space, programs and
 * context, and binds the component to the service. No cache maintenance and
 * no device access: OPEN publishes. Returns the component, or NULL with every
 * acquired page retained. */
struct pvr_mimus_service *a733_gpu_mimus_prepare(struct a733_gpu_mimus *, struct a733_gpu_reserved *,
    struct pvr_gpu_service *, uint32_t owner, const struct a733_gpu_mimus_layout *);
#endif
