/* The graph index that admission reads from the application's declared region (apps/os/npu-graph-index.c), on synthetic
 * regions: a valid index, and each way it can be wrong. */
#include "../apps/os/npu-graph-index.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint8_t region[8192];
static void put32(unsigned at, uint32_t v) { for (unsigned i = 0; i < 4; ++i) region[at + i] = (uint8_t)(v >> (8 * i)); }
static void put64(unsigned at, uint64_t v) { for (unsigned i = 0; i < 8; ++i) region[at + i] = (uint8_t)(v >> (8 * i)); }
static void build(unsigned count)
{
    memset(region, 0, sizeof region);
    put32(0, OS_NPU_INDEX_MAGIC); put32(4, 1); put64(8, count);
    for (unsigned i = 0; i < count; ++i) { put64(16 + 16 * i, 4096 + 1024 * i); put64(24 + 16 * i, 1000); }
}
int main(void)
{
    build(3);
    assert(os_npu_index_check(region, sizeof region, 3, 0) == OS_NPU_INDEX_OK);
    assert(os_npu_index_check(region, sizeof region, 3, 1000) == OS_NPU_INDEX_OK);
    uint64_t offset, bytes;
    os_npu_index_entry(region, 2, &offset, &bytes);
    assert(offset == 4096 + 2048 && bytes == 1000);
    assert(os_npu_index_check(region, sizeof region, 3, 999) == OS_NPU_INDEX_OVERSIZE);
    assert(os_npu_index_check(region, sizeof region, 2, 0) == OS_NPU_INDEX_COUNT);       /* the session declares another count */
    assert(os_npu_index_check(region, 15, 3, 0) == OS_NPU_INDEX_SHORT);
    assert(os_npu_index_check(NULL, sizeof region, 3, 0) == OS_NPU_INDEX_SHORT);
    build(3); region[0] ^= 1;
    assert(os_npu_index_check(region, sizeof region, 3, 0) == OS_NPU_INDEX_MAGIC_BAD);
    build(3); put32(4, 2);
    assert(os_npu_index_check(region, sizeof region, 3, 0) == OS_NPU_INDEX_VERSION_BAD);
    build(0);
    assert(os_npu_index_check(region, sizeof region, 0, 0) == OS_NPU_INDEX_COUNT);       /* no graph at all */
    build(3); put64(8, UINT64_MAX);
    assert(os_npu_index_check(region, sizeof region, UINT64_MAX, 0) == OS_NPU_INDEX_COUNT); /* more entries than the region holds */
    build(3); put64(24, 0);
    assert(os_npu_index_check(region, sizeof region, 3, 0) == OS_NPU_INDEX_BOUNDS);       /* an empty graph */
    build(3); put64(16, 40);
    assert(os_npu_index_check(region, sizeof region, 3, 0) == OS_NPU_INDEX_BOUNDS);       /* a graph inside the index */
    build(3); put64(16 + 16, sizeof region - 10);
    assert(os_npu_index_check(region, sizeof region, 3, 0) == OS_NPU_INDEX_BOUNDS);       /* past the end of the region */
    build(3); put64(16, UINT64_MAX - 100); put64(24, 200);
    assert(os_npu_index_check(region, sizeof region, 3, 0) == OS_NPU_INDEX_BOUNDS);       /* offset plus length wraps */
    puts("PASS NPU graph index: valid, wrong count, magic, version, empty, inside the index, past the end, wrap, oversize");
    return 0;
}
