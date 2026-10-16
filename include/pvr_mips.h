#ifndef SUNSTONE_PVR_MIPS_H
#define SUNSTONE_PVR_MIPS_H
#include "pvr_firmware.h"

#define PVR_MIPS_PAGE_BYTES 4096u
#define PVR_MIPS_TABLE_BYTES 16384u
#define PVR_MIPS_MAX_OBJECTS 64u
#define PVR_MIPS_WRITES 11u
#define PVR_MIPS_UNCACHED 2u
#define PVR_MIPS_WRITE_THROUGH 1u

/* CPU-contiguous storage backed by the listed GPU-visible physical pages.
 * Buffer order for preparation is code, data, then the four page-table pages.
 * The caller must establish these CPU/physical correspondences and ownership.
 */
struct pvr_mips_buffer {
    struct pvr_fw_buffer storage;
    const uint64_t *pages;
    uint32_t page_count;
};

/* Additional firmware objects, including queues and configuration structures.
 * Addresses are in the MIPS 16 MiB heap beginning at 0xc0000000. The allocator
 * must enforce reserved heap/configuration ownership. All entries are readable,
 * writable, and executable, matching the reference firmware object mappings.
 */
struct pvr_mips_object {
    const uint64_t *pages;
    uint32_t page_count, address, cache_policy;
};
struct pvr_mips_write { uint32_t offset, width; uint64_t value; };
struct pvr_mips_boot {
    uint32_t physical_address_bits, write_count;
    struct pvr_mips_write writes[PVR_MIPS_WRITES];
};

/* Prepare an offline A733 MIPS firmware instance. Revalidate the image and its
 * feature parameters, stage code/data, build the complete 16 KiB little-endian
 * page table, fill the 64-byte boot configuration, and return ordered wrapper
 * register writes. Does not execute those writes or start the GPU.
 *
 * All CPU outputs must be disjoint from each other and every input. Physical
 * pages must be distinct, aligned, below 2^36, and outside wrapper-intercepted
 * addresses. Exact buffer/page counts are required. Additional objects may use
 * uncached or write-through mappings. Their contents are not initialized here.
 * NULL objects is valid only when object_count is zero.
 *
 * Keep all inputs stable and outputs exclusively owned until return. CPU byte
 * buffers may be unaligned; typed records and page arrays require C alignment.
 * The platform must verify RAM ownership, physical aliases with input storage,
 * reserved regions, device addressability, and CPU/physical correspondence.
 * It must publish caches and install GPU memory contexts before execution.
 *
 * Returns 1 on success, 0 with all outputs unchanged on refusal. No allocation,
 * I/O, cache maintenance, MMIO, live page-table updates, or device qualification.
 */
int pvr_mips_prepare(const void *image, size_t bytes,
                     const struct pvr_mips_buffer buffers[3],
                     const struct pvr_mips_object *objects, size_t object_count,
                     struct pvr_mips_boot *boot);
#endif
