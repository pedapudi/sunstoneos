/* SPDX-License-Identifier: GPL-2.0-only */
/* The hardware facts application (docs/hardware/a733-facts.md): returns the table that every core filled at
 * EL1 before the scheduler started. It uses no device and writes no register. */
#include "hw_facts-app.h"
#include "a733_hw_facts.h"

/* Written by every core at EL1 before the scheduler (platform/a733/hw_facts.c). */
struct hwf_table hwf_table __attribute__((aligned(16), used)) = HWF_TABLE_INITIALIZER;

void hw_facts_main(const struct ss_grant *grant)
{
    (void)grant;
    const unsigned all = (1u << HWF_CORES) - 1;
    for (unsigned waits = 0; __atomic_load_n(&hwf_table.header.done_mask, __ATOMIC_ACQUIRE) != all; ++waits) {
        if (ss_live() < 0 || waits > 1000000) ss_fail(0xfac1);
        ss_progress();
    }
    ss_finish(&hwf_table, sizeof(hwf_table));
}
