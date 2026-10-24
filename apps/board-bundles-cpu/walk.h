/* SPDX-License-Identifier: Apache-2.0 */
/* Acceptance of one walk block of bundle 4 (main.c), kept free of FreeRTOS
 * and hardware so that checks/board-bundles-apps-model.c runs it. */
#ifndef SUNSTONE_BOARD_BUNDLES_CPU_WALK_H
#define SUNSTONE_BOARD_BUNDLES_CPU_WALK_H
#include "a733_cpu_pmu.h"

/* Each iteration makes one data walk, for its load, and one instruction
 * walk, for the fetch after its TLBI. After the last TLBI, the return path
 * runs from the 2 MiB block whose entry that iteration's instruction walk
 * filled, so it adds no walk. The calibration region before the block (TLBI
 * VMALLE1, DSB ISH, ISB, then end) shows the walks after a lone TLBI: no data
 * walk, and one instruction walk, the walk that a block counts once per
 * iteration. A block is accepted when DTLB_WALK and ITLB_WALK are both
 * implemented and did not overflow, each equals the iteration count, and the
 * calibration counted 0 and 1. */
static inline unsigned bb4_walk_accepted(const struct a733_cpu_pmu_record *record,
                                         const struct a733_cpu_pmu_record *calibration, uint32_t iterations)
{
    unsigned accepted = calibration->events[A733_CPU_PMU_DTLB_WALK] == 0 &&
        calibration->events[A733_CPU_PMU_ITLB_WALK] == 1;
    for (unsigned e = A733_CPU_PMU_DTLB_WALK; e <= A733_CPU_PMU_ITLB_WALK; ++e)
        accepted &= (record->supported >> e) & 1 && !((record->overflow >> e) & 1) &&
            (calibration->supported >> e) & 1 && !((calibration->overflow >> e) & 1) &&
            record->events[e] == iterations;
    return accepted;
}

#endif
