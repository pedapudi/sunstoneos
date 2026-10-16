#ifndef SUNSTONE_PVR_FIRMWARE_H
#define SUNSTONE_PVR_FIRMWARE_H
#include <stddef.h>
#include <stdint.h>

#define PVR_FW_A733_BVNC UINT64_C(0x00240038006800b7)
#define PVR_FW_MAX_IMAGE (4u * 1024u * 1024u)
#define PVR_FW_MAX_ALLOCATION (16u * 1024u * 1024u)
#define PVR_FW_MAX_LOADS 32u
#define PVR_FW_LAYOUTS 6u

struct pvr_fw_layout {
    uint32_t id, type, address, maximum_bytes, allocation_bytes, allocation_offset;
};
struct pvr_fw_load {
    uint32_t file_offset, allocation, destination, file_bytes, memory_bytes, flags;
};
struct pvr_fw_plan {
    uint64_t bvnc;
    uint32_t major, minor, build, entry;
    uint32_t device_info_offset, device_info_bytes;
    uint32_t device_info_counts[4];
    uint32_t allocation_bytes[2]; /* Code, then data. */
    uint32_t load_count;
    struct pvr_fw_layout layouts[PVR_FW_LAYOUTS]; /* Ordered by section ID 4..9. */
    struct pvr_fw_load loads[PVR_FW_MAX_LOADS];
};
struct pvr_fw_buffer { void *cpu; size_t bytes; };

/* Decode an immutable, caller-owned MIPS firmware image. This component supports
 * the observed A733 BVNC, info version 3, firmware ABI major 1, 4 KiB pages,
 * six MIPS layout sections, and bounded ELF32 load segments. Output is unchanged
 * on refusal and must not overlap the image. Unaligned source bytes are valid.
 * Returns 1 on success, 0 on refusal. No allocation, device access, or I/O.
 *
 * Successful decoding proves structural/load bounds, not firmware authenticity,
 * device compatibility measured on hardware, or runtime protocol compatibility.
 * Pin the image identity separately. Device feature arrays remain borrowed raw
 * metadata in the image; the plan bounds them but does not interpret all flags.
 */
int pvr_fw_requirements(const void *image, size_t bytes, uint64_t expected_bvnc,
                        struct pvr_fw_plan *out);

/* Revalidate the image before writing either exclusively owned offline buffer.
 * Exact planned sizes are required. Buffers must be disjoint from each other,
 * the image, and the two buffer records. Physical aliases are caller-owned.
 * Keep input bytes and records stable until return. Refusal preserves both
 * buffers. Success zeroes complete allocations and copies ELF load data.
 *
 * Staging does not apply MIPS boot parameters, create page tables, flush caches,
 * power the GPU, or authorize execution. No runtime filesystem is required.
 */
int pvr_fw_stage(const void *image, size_t bytes, uint64_t expected_bvnc,
                 const struct pvr_fw_buffer buffers[2]);
#endif
