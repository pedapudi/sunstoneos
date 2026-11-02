/* Test application of checks/qemu-os-boot.py for a manifest region in level-1 slot 1 above the EL0 window
 * (docs/designs/application-memory-layout.md). Its 64 MiB work region is larger than the 48 MiB window, so the build
 * placed it in the slot 1 area and the OS mapped it for this application alone. The input region holds a mode:
 *   mode 0: write a pattern to every 4 KiB of the region, read it back, and finish with {tag, address, bytes, fold}.
 *   mode 1: read the first byte after the region: a permission fault at EL0.
 *   mode 2: overflow the main stack, which the OS took from upper memory with a guard block below it: an EL0 fault in
 *           the guard block, which no space maps. */
#include "slot1_test-app.h"

#define TAG UINT64_C(0x534c4f5431)        /* "SLOT1" */

static uint64_t __attribute__((noinline)) burn(uint64_t depth)
{
    volatile uint8_t frame[512];
    frame[0] = (uint8_t)depth;
    return depth ? burn(depth - 1) + frame[0] : frame[0];
}

void slot1_test_main(const struct ss_grant *grant)
{
    const uint64_t mode = ((const uint64_t *)grant->regions[SLOT1_TEST_REGION_INPUT].base)[0];
    volatile uint64_t *big = grant->regions[SLOT1_TEST_REGION_BIG].base;
    const uint64_t bytes = grant->regions[SLOT1_TEST_REGION_BIG].bytes;
    if (mode == 1) {
        (void)*((volatile uint8_t *)big + bytes);
        ss_fail(0xe03);
    }
    if (mode == 2) {
        (void)burn(1000);                                   /* about 500 KiB of frames on an 8 KiB stack */
        ss_fail(0xe04);
    }
    uint64_t fold = 0;
    for (uint64_t i = 0; i < bytes / 8; i += 512) big[i] = UINT64_C(0x9e3779b97f4a7c15) * (i + 1);
    big[bytes / 8 - 1] = ~(uint64_t)0;
    for (uint64_t i = 0; i < bytes / 8; i += 512) fold ^= big[i];
    fold ^= big[bytes / 8 - 1];
    const uint64_t result[4] = {TAG, (uintptr_t)big, bytes, fold};
    ss_finish(result, sizeof result);
}
