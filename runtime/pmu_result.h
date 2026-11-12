/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SUNSTONE_PMU_RESULT_H
#define SUNSTONE_PMU_RESULT_H
#include <stdint.h>
/* A single main-core application activity window. The data-walk count is a
 * 32-bit modular delta; the window must contain fewer than 2^32 such events.
 * Cycles and the chained instruction count have 64-bit deltas. */
#define SUNSTONE_PMU_RESULT_MAGIC UINT64_C(0x313052554d505353)
struct sunstone_pmu_result {
    uint64_t magic, core, cycles, inst_retired, dtlb_walk, ticks;
};
_Static_assert(sizeof(struct sunstone_pmu_result) == 48, "PMU result prefix");
#endif
