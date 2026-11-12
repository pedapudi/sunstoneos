#include "npu-graph-index.h"
static uint64_t u64(const uint8_t *p)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < 8; ++i) v |= (uint64_t)p[i] << (8 * i);
    return v;
}
static uint32_t u32(const uint8_t *p)
{ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
enum os_npu_index_result os_npu_index_check(const uint8_t *region, uint64_t region_bytes, uint64_t declared_graphs,
                                            uint64_t graph_bytes_max)
{
    if (!region || region_bytes < 16) return OS_NPU_INDEX_SHORT;
    if (u32(region) != OS_NPU_INDEX_MAGIC) return OS_NPU_INDEX_MAGIC_BAD;
    const uint32_t version = u32(region + 4);
    if (version != 1 && version != 2) return OS_NPU_INDEX_VERSION_BAD;
    const uint64_t count = u64(region + 8);
    if (!count || count != declared_graphs || count > (region_bytes - 16) / 16) return OS_NPU_INDEX_COUNT;
    const uint64_t first = 16 + 16 * count;     /* the graphs lie behind the index */
    for (uint64_t i = 0; i < count; ++i) {
        const uint64_t offset = u64(region + 16 + 16 * i), bytes = u64(region + 24 + 16 * i);
        if (version == 2 && !bytes) {
            /* A copy names an earlier entry that is a graph. */
            if (offset >= i || !u64(region + 24 + 16 * offset)) return OS_NPU_INDEX_COPY;
            continue;
        }
        if (!bytes || offset < first || offset > region_bytes || bytes > region_bytes - offset) return OS_NPU_INDEX_BOUNDS;
        if (graph_bytes_max && bytes > graph_bytes_max) return OS_NPU_INDEX_OVERSIZE;
    }
    return OS_NPU_INDEX_OK;
}
void os_npu_index_entry(const uint8_t *region, unsigned graph, uint64_t *offset, uint64_t *bytes)
{
    *offset = u64(region + 16 + 16 * (uint64_t)graph);
    *bytes = u64(region + 24 + 16 * (uint64_t)graph);
}
int os_npu_index_copy(const uint8_t *region, unsigned graph, unsigned *original)
{
    if (u32(region + 4) != 2 || u64(region + 24 + 16 * (uint64_t)graph)) return 0;
    *original = (unsigned)u64(region + 16 + 16 * (uint64_t)graph);
    return 1;
}
