#ifndef SUNSTONE_A733_GPU_QUEUE_H
#define SUNSTONE_A733_GPU_QUEUE_H
#include "a733_gpu_reserved.h"
#include "pvr_compute_queue.h"
#include "pvr_compute_programs.h"
#include "sunstone_app.h"

#define A733_GPU_QUEUE_PAYLOAD_PAGES 21u
#define A733_GPU_QUEUE_MAPS (GPU_QUEUE_LEASES + A733_GPU_QUEUE_PAYLOAD_PAGES)
#define A733_GPU_QUEUE_TABLE_PAGES 32u
#define A733_GPU_QUEUE_JOB_GPU (UINT64_C(0xda00000000) + 0x1e0000u)
#define A733_GPU_QUEUE_PROGRAM_GPU (UINT64_C(0xe000000000) + 0x100000u)
/* Retained static storage, initialized once before scheduling. Context
 * creation acquires table pages in the worker; leases never clear user data. */
struct a733_gpu_queue {
    struct a733_gpu_reserved *firmware;
    struct device_memory_lease payload_lease, firmware_lease, table_lease;
    struct device_memory_extent payload_extent, firmware_extent, table_extent;
    struct pvr_compute_queue component;
    struct pvr_compute_program_config programs;
    struct pvr_compute_program_result program_result;
    struct a733_gpu_vm_memory vm;
    struct pvr_gpu_vm_requirements requirements;
    struct pvr_gpu_mapping maps[A733_GPU_QUEUE_MAPS];
    uint64_t payload_pages[A733_GPU_QUEUE_PAYLOAD_PAGES];
    uint64_t firmware_pages[PVR_COMPUTE_QUEUE_OBJECTS];
    uint64_t table_pages[A733_GPU_QUEUE_TABLE_PAGES];
    const struct ss_region *regions; /* the application's manifest table; it outlives the queue */
    uint32_t region_count, owner, stage;
};
struct pvr_compute_queue *a733_gpu_queue_prepare(struct a733_gpu_queue *, struct a733_gpu_reserved *,
    struct pvr_gpu_service *, uint32_t owner, const struct ss_region *, uint32_t region_count);
int a733_gpu_queue_create(struct a733_gpu_queue *, const struct gpu_queue_context_config *);
int a733_gpu_queue_reopen(struct a733_gpu_queue *);
#endif
