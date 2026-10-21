/* AArch64 entry points for checks/remote-writers-model.py. The read callback
 * performs one aligned 32-bit load, as a monitor with the MMU off would. */
#include "a733_remote_writers.h"

volatile uint64_t remote_writers_fail_address; /* A read here reports failure. */

static int mmio_read(void *context, uint64_t address, uint32_t *value)
{
    (void)context;
    if (address == remote_writers_fail_address) return 0;
    *value = *(volatile const uint32_t *)(uintptr_t)address;
    return 1;
}

uint32_t remote_writers_mmio(void)
{
    const struct a733_remote_writers_ops ops = {mmio_read, 0};
    return a733_remote_writers_excluded(&ops);
}

uint32_t remote_writers_null_ops(void)
{
    return a733_remote_writers_excluded(0);
}

uint32_t remote_writers_null_read(void)
{
    const struct a733_remote_writers_ops ops = {0, 0};
    return a733_remote_writers_excluded(&ops);
}
