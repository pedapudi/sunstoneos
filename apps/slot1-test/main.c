/* slot1-test: a manifest region in level-1 slot 1 above the EL0 window, and the main stack's guard block
 * (docs/designs/application-memory-layout.md). The 64 MiB work region does not fit the 48 MiB window, so the build
 * placed it at 0x48000000; the OS maps it for this application alone and takes the main stack from the same memory,
 * a 2 MiB guard block and the stack above it, behind the region.
 *
 * SLOT1_TEST_MODE (the CMake cache variable SUNSTONE_SLOT1_TEST_MODE):
 *   0  write a pattern to the region, read it back and finish with {tag, address, bytes, fold}.
 *   1  the same write and read-back, ending in ss_fail(0x1e10) on a difference, then a read of the first byte after
 *      the region: the guard block, which no space maps, so an EL0 permission fault at level 2.
 *   2  overflow the main stack: an EL0 write fault in the guard block just below the stack. */
#include "slot1_test-app.h"

#ifndef SLOT1_TEST_MODE
#define SLOT1_TEST_MODE 0
#endif
#define TAG UINT64_C(0x534c4f5431)        /* "SLOT1" */
#define PATTERN UINT64_C(0x9e3779b97f4a7c15)

static uint64_t __attribute__((noinline)) burn(uint64_t depth)
{
    volatile uint8_t frame[512];
    frame[0] = (uint8_t)depth;
    return depth ? burn(depth - 1) + frame[0] : frame[0];
}

void slot1_test_main(const struct ss_grant *grant)
{
    volatile uint64_t *big = grant->regions[SLOT1_TEST_REGION_BIG].base;
    const uint64_t bytes = grant->regions[SLOT1_TEST_REGION_BIG].bytes;
#if SLOT1_TEST_MODE == 2
    (void)burn(1000);                                       /* about 500 KiB of frames on an 8 KiB stack */
    ss_fail(0x1e11);
#else
    uint64_t written = 0, read = 0;
    for (uint64_t i = 0; i < bytes / 8; i += 512) { big[i] = PATTERN * (i + 1); written ^= PATTERN * (i + 1); if (!(i & 0xffff)) ss_progress(); }
    big[bytes / 8 - 1] = ~(uint64_t)0;
    written ^= ~(uint64_t)0;
    for (uint64_t i = 0; i < bytes / 8; i += 512) read ^= big[i];
    read ^= big[bytes / 8 - 1];
    if (read != written) ss_fail(0x1e10);
#if SLOT1_TEST_MODE == 1
    (void)*((volatile uint8_t *)big + bytes);
    ss_fail(0x1e12);
#else
    const uint64_t result[4] = {TAG, (uintptr_t)big, bytes, read};
    ss_finish(result, sizeof result);
#endif
#endif
}
