#ifndef SUNSTONE_A733_NPU_MMU_H
#define SUNSTONE_A733_NPU_MMU_H
#include <stddef.h>
#include <stdint.h>

#define A733_NPU_MMU_MAX_REGIONS 512u
#define A733_NPU_MMU_MAX_BYTES (4096u + 256u * 16384u)

enum a733_npu_page_size {
    A733_NPU_PAGE_4K = 12,
    A733_NPU_PAGE_64K = 16,
    A733_NPU_PAGE_1M = 20,
    A733_NPU_PAGE_16M = 24
};

struct a733_npu_region {
    uint64_t physical;
    uint64_t bytes;
    uint32_t device;
    enum a733_npu_page_size page_shift;
    unsigned writable; /* 0: readable, 1: readable and writable */
};

struct a733_npu_mmu_image {
    uint64_t physical;
    size_t bytes;
    uint32_t pd_entry; /* Value for the PD-mode directory register at +0x3b4. */
    unsigned tables;
};

/* Build an OFFLINE, exclusively owned translation image for 32-bit device VA
 * and 40-bit PA. No allocation, cache maintenance, MMIO, or locking occurs.
 * The caller must pin the arena and payload memory, verify their physical
 * addresses, and prevent any device access to the arena throughout this call.
 * The caller must also keep regions stable for the duration of the call.
 *
 * Arena CPU and physical bases must be 4096-byte aligned. Capacity must be a
 * multiple of 64 bytes, <= A733_NPU_MMU_MAX_BYTES, and wholly in one 4 GiB PA
 * region. The entire capacity is reserved from device payload mappings.
 * Directory entries cover 16 MiB each and cannot mix page sizes. Each mapping
 * has aligned, nonzero length; device address zero is reserved as a guard.
 * Device ranges cannot overlap. Physical aliases are allowed only when both
 * mappings are read-only. Table, input metadata, and result CPU ranges cannot
 * overlap. Unregistered CPU aliases remain the platform's responsibility.
 *
 * Returns 1 on success, 0 on refusal. Refusal changes neither arena nor result.
 * Success writes only result->bytes of arena; unused entries cause exceptions.
 * The result is NOT a submission capability. Before hardware use, clean table
 * memory, order writes, establish device identity and quiescence, and program
 * the MMU with a qualified TLB invalidation sequence. Never rebuild a live table.
 * Internal SRAM windows and command-stream flush storage need separate setup. */
int a733_npu_mmu_build(void *arena, size_t capacity, uint64_t physical,
                       const struct a733_npu_region *regions, size_t count,
                       struct a733_npu_mmu_image *result);
#endif
