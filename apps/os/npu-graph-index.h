#ifndef SUNSTONE_OS_NPU_GRAPH_INDEX_H
#define SUNSTONE_OS_NPU_GRAPH_INDEX_H
/* The index at the start of the input region that holds an application's compiled NPU graphs (tools/npu-external-pack.py).
 * The session's NPU record names the region (graph_region, apps/os/npu-session.h); admission reads the index, copies every
 * graph out of the region into kernel memory and prepares the runtime from the copies, so the pool is sized from the graphs.
 *
 * Layout, little endian: "NMDL", u32 version (1), u64 count, then count pairs of u64 {offset from the start of the region,
 * length}. Each graph lies wholly inside the region, after the index, and is not empty. This unit reads only the region it is
 * given; it has no other dependency, so a host check can run it on synthetic regions. */
#include <stddef.h>
#include <stdint.h>
#define OS_NPU_INDEX_MAGIC UINT32_C(0x4c444d4e)
enum os_npu_index_result {
    OS_NPU_INDEX_OK, OS_NPU_INDEX_SHORT, OS_NPU_INDEX_MAGIC_BAD, OS_NPU_INDEX_VERSION_BAD, OS_NPU_INDEX_COUNT,
    OS_NPU_INDEX_BOUNDS, OS_NPU_INDEX_OVERSIZE
};
/* Check the index of a region of region_bytes against the graph count the session declares and the largest graph it allows
 * (zero: no upper bound). OS_NPU_INDEX_OK means every entry of 0..count-1 is readable with os_npu_index_entry. */
enum os_npu_index_result os_npu_index_check(const uint8_t *region, uint64_t region_bytes, uint64_t declared_graphs,
                                            uint64_t graph_bytes_max);
void os_npu_index_entry(const uint8_t *region, unsigned graph, uint64_t *offset, uint64_t *bytes);
#endif
