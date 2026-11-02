#ifndef SUNSTONE_A733_GPU_QUEUE_H
#define SUNSTONE_A733_GPU_QUEUE_H
#include "a733_gpu_reserved.h"
#include "pvr_compute_queue.h"
#include "pvr_compute_programs.h"
#include "sunstone_app.h"

/* The payload is the job slots, then the program pages, then the context-switch
 * and shared program images.
 * - Job pages: the retained job slots plus the first cache line of the uncached
 *   variant, in whole pages. They map at A733_GPU_QUEUE_JOB_GPU, whose 2 MiB
 *   region also holds the firmware's context-switch programs (PDS programs
 *   from 0xb0000, USC programs at 0x10000 and 0x60000); the pages after
 *   JOB_GPU + 0x1e0000 reach the region's end at 32, so a deeper queue would
 *   need a second leaf table and a placement that avoids the heap's firmware
 *   objects.
 * - Program pages: GPU_QUEUE_PROGRAM_PAGES, the application's USC code.
 * - Program images: the ten fixed regions of pvr_compute_program_bytes, 18
 *   pages in total (a733_gpu_queue_prepare verifies the sum). */
#define A733_GPU_QUEUE_JOB_BYTES (64u + PVR_COMPUTE_FAST_DEPTH * PVR_COMPUTE_FAST_STRIDE)
#define A733_GPU_QUEUE_JOB_PAGES ((A733_GPU_QUEUE_JOB_BYTES + 4095u) / 4096u)
#define A733_GPU_QUEUE_JOB_PAGES_MAX 32u
GPU_QUEUE_STATIC_ASSERT(A733_GPU_QUEUE_JOB_PAGES <= A733_GPU_QUEUE_JOB_PAGES_MAX,
    "the job slots exceed the 2 MiB region they share with the context-switch programs: lower the depth, bindings or argument bytes");
#define A733_GPU_QUEUE_PROGRAM_IMAGE_PAGES 18u
#define A733_GPU_QUEUE_PAYLOAD_PAGES \
    (A733_GPU_QUEUE_JOB_PAGES + GPU_QUEUE_PROGRAM_PAGES + A733_GPU_QUEUE_PROGRAM_IMAGE_PAGES)
#define A733_GPU_QUEUE_MAPS (GPU_QUEUE_LEASES + A733_GPU_QUEUE_PAYLOAD_PAGES)
#define A733_GPU_QUEUE_TABLE_PAGES GPU_QUEUE_TABLE_PAGES
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
