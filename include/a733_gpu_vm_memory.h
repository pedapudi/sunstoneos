#ifndef SUNSTONE_A733_GPU_VM_MEMORY_H
#define SUNSTONE_A733_GPU_VM_MEMORY_H
#include "pvr_gpu_vm.h"

enum a733_gpu_vm_memory_phase { A733_GPU_VM_MEMORY_NEW, A733_GPU_VM_MEMORY_OFFLINE,
    A733_GPU_VM_MEMORY_PREPARED, A733_GPU_VM_MEMORY_PUBLISHING,
    A733_GPU_VM_MEMORY_PUBLISHED, A733_GPU_VM_MEMORY_RECLAIMING,
    A733_GPU_VM_MEMORY_FAILED };
enum a733_gpu_vm_memory_result { A733_GPU_VM_MEMORY_OK, A733_GPU_VM_MEMORY_AGAIN,
    A733_GPU_VM_MEMORY_INVALID, A733_GPU_VM_MEMORY_FAULT };
struct a733_gpu_vm_memory {
    struct pvr_gpu_vm_tables tables;
    void *context_cpu;
    uint64_t context_physical;
    uint32_t cursor;
    enum a733_gpu_vm_memory_phase phase;
};

/* Zero once; one privileged owner serializes all calls at EL1. The platform
 * reserves/pins RAM and excludes aliases with all live allocations. AT checks
 * establish CPU/PA correspondence and access rights, not allocator ownership.
 * Tables require page-aligned contiguous CPU storage, strictly increasing 4 KiB
 * PA entries, Normal write-back (MAIR ff), inner-shareable mappings. The 32-byte
 * firmware context is 8-byte aligned within one Normal Non-cacheable (MAIR 44)
 * shareable page, without any cacheable alias. Its entire physical page must be
 * excluded from workload data. No stage-2 remapping is permitted.
 * Init copies descriptors; the immutable PA list remains pinned and disjoint.
 * Refusal leaves state/backing unchanged. No allocation, MMIO or cache writes.
 */
int a733_gpu_vm_memory_init(struct a733_gpu_vm_memory *, const struct pvr_gpu_vm_tables *,
                            void *context_cpu, uint64_t context_physical);

/* OFFLINE only: validate CPU mappings, exclude context-page data aliases, and
 * call the existing offline builder. Invalid maps leave outputs/state unchanged;
 * lost CPU mappings latch FAILED. All table/context bytes remain private until
 * publication completes. Maps must not alias adapter state. Data allocation,
 * permissions, GPU cache policy, and data publication remain platform duties.
 */
enum a733_gpu_vm_memory_result a733_gpu_vm_memory_build(struct a733_gpu_vm_memory *,
    const struct pvr_gpu_mapping *, size_t count);

/* PREPARED only. Poll validates and cleans one table page to PoC per call, then
 * validates and orders the uncached context. Only OK/PUBLISHED permits device
 * attachment. The owner freezes all mappings/backing during publication and
 * device use; the GPU must not write table pages. No translation-cache flush,
 * firmware context registration, job submission, or deadline is supplied here.
 * The service must enforce its deadline between bounded page steps.
 */
enum a733_gpu_vm_memory_result a733_gpu_vm_memory_publish_begin(struct a733_gpu_vm_memory *);
enum a733_gpu_vm_memory_result a733_gpu_vm_memory_poll(struct a733_gpu_vm_memory *);

/* PUBLISHED only, AFTER independent proof that GPU access is blocked/drained
 * and the firmware context is detached. Begin does not establish isolation.
 * Poll invalidates one table page per call before returning to OFFLINE. A
 * terminal mapping/cache failure retains backing; later calls perform no
 * translation/cache/shared-memory operations. No forced release/retry API.
 */
enum a733_gpu_vm_memory_result a733_gpu_vm_memory_reclaim_begin(struct a733_gpu_vm_memory *);
#endif
