/* SPDX-License-Identifier: Apache-2.0 */
/* Acceptance of one walk block of bundle 4 (main.c), kept free of FreeRTOS
 * and hardware so that checks/board-bundles-apps-model.c runs it on the
 * board's records. */
#ifndef SUNSTONE_BOARD_BUNDLES_CPU_WALK_H
#define SUNSTONE_BOARD_BUNDLES_CPU_WALK_H
#include "a733_cpu_pmu.h"

/* A block is accepted when DTLB_WALK and ITLB_WALK are both implemented, did
 * not overflow and each equal the iteration count. Each iteration makes one
 * data walk, for its load, and one instruction walk, for the fetch after its
 * TLBI. After the last TLBI, the return path runs from the 2 MiB block whose
 * entry that iteration's instruction walk filled, so it adds no walk. The
 * calibration region's one instruction walk is the walk of the fetch after its
 * own TLBI; in a block, that walk is already counted once per iteration. */
static inline unsigned bb4_walk_accepted(const struct a733_cpu_pmu_record *record, uint32_t iterations)
{
    unsigned accepted = 1;
    for (unsigned e = A733_CPU_PMU_DTLB_WALK; e <= A733_CPU_PMU_ITLB_WALK; ++e)
        accepted &= (record->supported >> e) & 1 && !((record->overflow >> e) & 1) &&
            record->events[e] == iterations;
    return accepted;
}

#endif
