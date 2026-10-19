#ifndef SUNSTONE_A733_GPU_MEMORY_H
#define SUNSTONE_A733_GPU_MEMORY_H
#include "pvr_fw_state.h"
#include "pvr_ccb.h"

enum a733_gpu_memory_phase { A733_GPU_MEMORY_NEW, A733_GPU_MEMORY_OFFLINE,
    A733_GPU_MEMORY_PUBLISHED, A733_GPU_MEMORY_FAILED };
struct a733_gpu_memory {
    struct pvr_mips_buffer firmware[3], resources[PVR_FW_RESOURCE_COUNT];
    enum a733_gpu_memory_phase phase;
};

/* Privileged EL1 memory adapter. Zero once, initialize before the service.
 * Buffers are page-aligned, CPU-contiguous, backed by immutable PA lists.
 * Code/data/tables require Normal write-back, inner-shareable CPU mappings
 * (MAIR 0xff). All nineteen shared resources require Normal Non-cacheable
 * mappings (MAIR 0x44, outer/inner shareable); their fields have mixed CPU/GPU
 * ownership within cache lines. No cacheable alias may exist for those pages.
 * The platform reserves RAM, excludes other users/aliases, and pins mappings;
 * translation checks cannot establish allocator ownership or absence of aliases.
 * Refusal preserves state/backing. Success copies descriptors, retaining page
 * lists. No allocation, MMIO, device power, reset, or firmware execution occurs.
 */
int a733_gpu_memory_init(struct a733_gpu_memory *, const struct pvr_mips_buffer firmware[3],
                         const struct pvr_mips_buffer resources[PVR_FW_RESOURCE_COUNT]);

/* Serialized worker callbacks. Any refusal latches FAILED, retaining all
 * backing; subsequent callbacks perform no translation/cache/memory operation.
 * Output arguments are unchanged on refusal. Inputs/outputs must be private CPU
 * memory outside this adapter, its shared buffers, and immutable PA lists.
 * Four-byte queue accesses are single aligned word transactions. Other shared
 * reads use volatile byte loads. Full-system barriers order accesses.
 * Queue writes are unrestricted during OFFLINE formatting. While PUBLISHED,
 * only host producer, host commands, return clearing, and event consumer writes
 * are permitted. Physical coherency and transaction visibility remain unqualified.
 */
int a733_gpu_memory_read(void *, enum pvr_ccb_region, uint32_t, void *, size_t);
int a733_gpu_memory_write(void *, enum pvr_ccb_region, uint32_t, const void *, size_t);
int a733_gpu_memory_barrier(void *, enum pvr_ccb_barrier);
int a733_gpu_memory_publish(void *);
int a733_gpu_memory_started(void *, uint32_t *);
int a733_gpu_memory_compat(void *, uint32_t, void *, size_t);
int a733_gpu_memory_sync_write(void *, uint32_t);
int a733_gpu_memory_sync_read(void *, uint32_t *);
/* Acquire firmware system-data power state; zero means ready for power-down. */
int a733_gpu_memory_power_state(void *, uint32_t *);

/* Call only AFTER the complete healthy firmware/local-idle/reset/platform-OFF
 * sequence establishes orderly shutdown. Failed/uncertain DMA requires separate
 * physical recovery. This function does not establish either condition.
 * Invalidate cached firmware backing and return to OFFLINE for re-preparation.
 * A failed instance cannot be reclaimed through this API. Never access cached
 * firmware backing while PUBLISHED. Shared resources stay uncached throughout.
 */
int a733_gpu_memory_reclaim(struct a733_gpu_memory *);
#endif
