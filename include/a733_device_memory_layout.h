#ifndef SUNSTONE_A733_DEVICE_MEMORY_LAYOUT_H
#define SUNSTONE_A733_DEVICE_MEMORY_LAYOUT_H
#include <stdint.h>
#include <stddef.h>
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
#define A733_DEVICE_MEMORY_BATCH_PAGES 64u
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
 * immutable linker geometry; refuse any second admission. Protected table
 * construction must consume this same binding before cache publication.
 * External DRAM qualification, resolved aliases and all-master exclusion remain
 * mandatory launch prerequisites. This routine does not manufacture them. */
int a733_device_memory_boot_admit(const struct a733_smp_contract *);
int a733_device_memory_tables_admitted(struct a733_device_memory_layout *);
int a733_device_memory_tables_complete(void);
/* Primary EL1 only, after the real eight-core barrier and before kernel release.
 * Each call verifies at most 64 complete pages in both read/write directions.
 * Returns 0 pending, 1 published, -1 refused/retained. The actual startup deadline
 * applies before and after each batch. No allocation, clearing, DMA or MMIO.
 * Both singleton pools publish together only after every translation passes. */
int a733_device_memory_prepare(void);
/* Callback-free retained publication state for native pool getters. All access
 * remains privileged and cooperative. It does not authorize EL0 payload access,
 * master reachability, cache coherence or device drain. */
int a733_device_memory_ready(void);
/* Internal singleton commit guard, valid only inside preparation after both
 * complete mapping scans. The native pool owners use it before initialization. */
int a733_device_memory_commit_allowed(unsigned nc, uintptr_t start, uintptr_t end);
#endif
#endif
