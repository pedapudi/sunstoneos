/* Test application of checks/qemu-os-boot.py: its main stores to an address
 * that no translation table entry maps (invalid at both EL0 and EL1), so that
 * the task takes a data abort at EL0 (docs/designs/failure-return.md). The
 * vector must abandon the task. The statement after the store is never run. */
#include "el0_abort-app.h"

void el0_abort_main(const struct ss_grant *grant)
{
    (void)grant;
    *(volatile uint32_t *)(uintptr_t)UINT32_C(0x7a000000) = 0x41424f52;
    ss_fail(0xe99);
}
