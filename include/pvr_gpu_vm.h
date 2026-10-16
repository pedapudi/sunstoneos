#ifndef SUNSTONE_PVR_GPU_VM_H
#define SUNSTONE_PVR_GPU_VM_H
#include <stddef.h>
#include <stdint.h>

#define PVR_GPU_VM_READ_ONLY 1u
#define PVR_GPU_VM_COHERENT 2u
#define PVR_GPU_VM_SLC_BYPASS 4u
#define PVR_GPU_VM_FW_PROTECT 8u
#define PVR_GPU_VM_CONTEXT_BYTES 32u

/* One physically contiguous span. Sort spans by ascending virtual address;
 * split scatter/gather allocations into spans. Supported page shifts are
 * 12, 14, 16, 18, 20 and 21. A single 2 MiB virtual region must use one page
 * size, including disjoint spans within that region. All mappings permit reads;
 * writes are allowed unless READ_ONLY or firmware/parameter-manager protection
 * restricts them. The hardware format has no execute-disable bit here.
 */
struct pvr_gpu_mapping {
    uint64_t address, physical, bytes;
    uint32_t page_shift, flags;
};
struct pvr_gpu_vm_requirements {
    uint64_t table_bytes, mapped_bytes;
    uint32_t table_pages, directory_pages, leaf_tables, mapped_pages;
};
struct pvr_gpu_vm_tables {
    void *cpu;
    size_t bytes;
    const uint64_t *physical_pages;
    uint32_t page_count;
};

/* A733 profile: 40-bit virtual and 36-bit physical addresses. Validate spans
 * and report exact table storage. Empty maps are valid and need one root page.
 * Output is unchanged on refusal and must not overlap the input spans.
 */
int pvr_gpu_vm_require(const struct pvr_gpu_mapping *maps, size_t count,
                        struct pvr_gpu_vm_requirements *out);

/* Construct an exclusively owned offline GPU address space. tables must have
 * the exact required size and page count. Backing pages must be 4 KiB aligned,
 * nonzero, below 2^36, and strictly increasing; CPU storage maps them in that
 * order. They may be discontiguous. No backing page may intersect mapped data.
 * All table pages are zeroed, then valid entries are populated in little-endian
 * order. Unmapped entries remain invalid. Each leaf table owns a full 4 KiB
 * backing page even when its selected page size uses fewer entries.
 *
 * context is an exact 32-byte firmware memory-context record. It receives the
 * root physical address and an unallocated catalogue-register marker. The
 * service must allocate/map this record in firmware memory separately.
 *
 * Inputs must remain stable, CPU outputs must be disjoint from each other and
 * all input records, and callers must provide writable output spans. Offline
 * CPU bytes may be unaligned; typed input records require normal C alignment.
 * The platform owns RAM admission, physical aliases with input/context storage,
 * actual CPU/physical correspondence, and cache publication. Physical aliases
 * among data spans are permitted; admission must enforce consistent ownership,
 * protection, and cache policy across those aliases. COHERENT encodes a bit;
 * it does not establish platform coherence.
 *
 * Returns 1 on success; 0 preserves all output bytes. No allocation, MMIO,
 * device attachment, live mapping change, or translation-cache invalidation.
 * Never replace or reuse device-visible tables until device access has stopped
 * or the required invalidation and completion protocol has been verified.
 */
int pvr_gpu_vm_build(const struct pvr_gpu_mapping *maps, size_t count,
                      const struct pvr_gpu_vm_tables *tables, void *context, size_t context_bytes);
#endif
