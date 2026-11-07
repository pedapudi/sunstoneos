/* Test application of checks/qemu-os-boot.py for a manifest region in level-1 slot 1 above the EL0 window
 * (docs/designs/application-memory-layout.md). Its 64 MiB work region is larger than the 48 MiB window, so the build
 * placed it in the slot 1 area and the OS mapped it for this application alone. The input region holds a mode:
 *   mode 0: write a pattern to every 4 KiB of the region, read it back, and finish with {tag, address, bytes, fold}.
 *   mode 1: read the first byte after the region: a permission fault at EL0.
 *   mode 2: overflow the main stack, which the OS took from upper memory with a guard block below it: an EL0 fault in
 *           the guard block, which no space maps.
 *   mode 3: the init hook (which the OS calls before the scheduler, where no task creates anything) creates a task. The
 *           main task asks it to store to the main task's own stack, which only the application's space maps, and to the
 *           region; the child must run in the application's space, with no call. The result is {tag, stack word, region
 *           word, the child's task tag}. */
#include "slot1_test-app.h"
#include "FreeRTOS.h"
#include "task.h"

#define TAG UINT64_C(0x534c4f5431)        /* "SLOT1" */

static uint64_t __attribute__((noinline)) burn(uint64_t depth)
{
    volatile uint8_t frame[512];
    frame[0] = (uint8_t)depth;
    return depth ? burn(depth - 1) + frame[0] : frame[0];
}

#define TAG_INIT UINT64_C(0x534c4f5433)   /* "SLOT3" */
static StaticTask_t child_state;
static StackType_t child_stack[512];
static volatile uint64_t *volatile stack_target, *volatile region_target;
static volatile uint64_t child_done, child_tag;

static void child(void *unused)
{
    (void)unused;
    while (!stack_target) vTaskDelay(1);
    *stack_target = UINT64_C(0x1111222233334444);       /* the main task's ledger stack: only the application's space maps it */
    *region_target = UINT64_C(0x5555666677778888);
    child_tag = (uintptr_t)xTaskGetApplicationTaskTag(NULL);
    child_done = 1;
    for (;;) vTaskDelay(1000);
}

/* Runs once before the scheduler, from os_application_prepare. */
void slot1_test_init(void)
{
    (void)xTaskCreateStaticAffinitySet(child, "init-child", 512, NULL, 3, child_stack, &child_state, 0xffu);
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
    if (mode == 3) {
        volatile uint64_t slot = 0;
        region_target = big;
        stack_target = &slot;
        while (!child_done) vTaskDelay(1);
        const uint64_t result[4] = {TAG_INIT, slot, big[0], child_tag};
        ss_finish(result, sizeof result);
    }
    uint64_t fold = 0;
    for (uint64_t i = 0; i < bytes / 8; i += 512) big[i] = UINT64_C(0x9e3779b97f4a7c15) * (i + 1);
    big[bytes / 8 - 1] = ~(uint64_t)0;
    for (uint64_t i = 0; i < bytes / 8; i += 512) fold ^= big[i];
    fold ^= big[bytes / 8 - 1];
    const uint64_t result[4] = {TAG, (uintptr_t)big, bytes, fold};
    ss_finish(result, sizeof result);
}
