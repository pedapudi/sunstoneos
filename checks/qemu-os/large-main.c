/* Test application of checks/qemu-os-boot.py for large regions (docs/designs/large-app-regions.md). It declares
 * large regions in its manifest and finishes at once: the check looks at the OS's admission of them. */
#include "large_test-app.h"

void large_test_main(const struct ss_grant *grant)
{
    (void)grant;
    const uint64_t result[2] = {0x4c415247, 0};
    ss_finish(result, sizeof(result));
}
