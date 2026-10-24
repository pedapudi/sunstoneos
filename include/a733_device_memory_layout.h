#ifndef SUNSTONE_A733_DEVICE_MEMORY_LAYOUT_H
#define SUNSTONE_A733_DEVICE_MEMORY_LAYOUT_H
#include <stdint.h>
#include <stddef.h>
#include "a733_dram.h"
/* The large NPU graph uses existing identity-mapped image RAM. This fixed
 * exception grants no external-memory, cache, device or physical authority. */
#if defined(SUNSTONE_NPU_LARGE_GRAPH)
#if !defined(SUNSTONE_NPU_SINGLE_CORE) || !defined(SUNSTONE_NPU_RUNTIME_INPUT) || \
    !defined(SUNSTONE_NPU_INTERRUPT) || !defined(SUNSTONE_NPU_SUPERVISED) || \
    defined(SUNSTONE_NPU_FINITE_RETURN) || defined(SUNSTONE_A733_EXTENDED_DEVICE_MEMORY)
#error Large NPU graphs require the retained single-core IRQ runtime memory profile
#endif
#define A733_NPU_GRAPH_POOL_START UINT64_C(0x41600000)
#define A733_NPU_GRAPH_POOL_END UINT64_C(0x41a00000)
#define A733_LOCAL_DEVICE_POOL_BYTES UINT32_C(4194304)
/* Geometry only. Both EL3 table preparation and the EL1 allocator use this
 * predicate; their state, mapping and ownership checks remain separate. */
static inline int a733_device_memory_local_pool_valid(uintptr_t start, uintptr_t end)
{
    return start == A733_NPU_GRAPH_POOL_START && end == A733_NPU_GRAPH_POOL_END;
}
#else
#define A733_LOCAL_DEVICE_POOL_BYTES UINT32_C(2097152)
#endif
#define A733_DEVICE_MEMORY_LAYOUT 1u
#define A733_DEVICE_WB_START UINT64_C(0x48000000)
#define A733_DEVICE_WB_END UINT64_C(0x78000000)
#define A733_DEVICE_NC_START UINT64_C(0x78000000)
#define A733_DEVICE_NC_END UINT64_C(0x79000000)
/* The EL1 tables map both extended pools with 2 MiB level-2 blocks. */
#define A733_DEVICE_MEMORY_BLOCK ((size_t)2 << 20)
/* EL1 stage-1 block descriptor for DRAM beyond the boot image: physical
 * address pa, UXN and PXN, access flag, inner shareable (SH = 0b11), MAIR
 * index 1 (Normal write-back) and AP = 0b00 (EL1 read/write, no EL0 access).
 * Level-1 (1 GiB) and level-2 (2 MiB) blocks share the format. The cached
 * builder's DRAM map above the boot image and the memory qualification
 * image's tested map both use it. */
#define A733_EL1_DRAM_BLOCK(pa) ((uint64_t)(pa) | (UINT64_C(1) << 54) | (UINT64_C(1) << 53) | \
    (UINT64_C(1) << 10) | (UINT64_C(3) << 8) | (UINT64_C(1) << 2) | 1)
/* The top of DRAM that the EL1 tables can map: 12 GiB from 0x40000000. The
 * cached builder maps DRAM above the boot image only when
 * a733_dram_decode_check() of a733_dram.h passes for those ranges. */
#define A733_DRAM_MAP_END UINT64_C(0x340000000)
/* The address step of the local pool check, a733_memory_local_check() in
 * a733_memory_critical.h. An image whose tables map every local pool byte
 * with 2 MiB level-2 block descriptors defines it as 2 MiB:
 * platform/a733-smp-cached/memory.c and platform/a733/platform.c do. The
 * weak default beside the check, 4096, checks every page. A file that defines
 * it must not include a733_memory_critical.h. */
extern const uintptr_t a733_local_pool_stride;
struct a733_device_memory_layout {
    uint64_t profile, wb_start, wb_end, nc_start, nc_end;
};
/* Exact immutable layout shape, not RAM discovery or physical qualification. */
int a733_device_memory_layout_valid(const struct a733_device_memory_layout *);
/* v2 keeps the boot contract's 224-byte ABI. Its six former reserved words are
 * profile, external DRAM reservation identity, WB start/end, NC start/end.
 * The surrounding boot record authenticates build/epoch/generation/boot lease.
 * The launch coordinator must verify actual physical ownership evidence for
 * these exact intervals; nonzero words alone never establish that evidence. */
int a733_device_memory_handoff_valid(const struct a733_device_memory_layout *, const uint64_t words[6]);
#if defined(SUNSTONE_A733_EXTENDED_DEVICE_MEMORY)
struct a733_smp_contract;
/* Actual primary EL3 cold boot only. Validate the copied live v2 contract and
 * immutable linker geometry; refuse any second admission. External DRAM
 * qualification, resolved aliases and all-master exclusion remain mandatory
 * launch prerequisites. This routine does not manufacture them. */
int a733_device_memory_boot_admit(const struct a733_smp_contract *);
/* Transitional table-builder interface: the builder reads the admitted layout
 * and reports completion. Once the builder reads the layout constants above
 * directly, these two calls are removed. */
int a733_device_memory_tables_admitted(struct a733_device_memory_layout *);
int a733_device_memory_tables_complete(void);
/* One call on core 0 at EL1, after the eight-core barrier and before the
 * scheduler starts. It prepares the `dma32` and `uncached` pools (one write
 * translation per 2 MiB block, 392 in all), then checks the installed table
 * base, TCR, MAIR and the admitted epoch and deadline once, and sets READY. It
 * returns 1 when READY, -1 on refusal; a refusal is retained, and a pool
 * initialized before a refused check stays unreachable because the getters
 * test READY. Other cores and later calls change nothing. No allocation,
 * clearing, DMA or MMIO. */
int a733_device_memory_prepare(void);
/* True once preparation has succeeded. The EL1 tables do not change after
 * preparation, so READY also marks the final mapping. Starting the scheduler
 * orders it before every task. It does not authorize EL0 access, master
 * reachability, cache coherence or device drain. */
int a733_device_memory_ready(void);
#endif
#endif
