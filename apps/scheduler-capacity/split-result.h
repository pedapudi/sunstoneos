/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SCHEDULER_CAPACITY_SPLIT_RESULT_H
#define SCHEDULER_CAPACITY_SPLIT_RESULT_H
#include "result.h"
#define SPLIT_MAGIC UINT64_C(0x53534353504c3031)
#define SPLIT_UNITS 256u
#define SPLIT_WARM 1u
#define SPLIT_REPS 3u
/* Each writer owns a complete cache line. Times use the generic counter. */
struct split_core {
    uint64_t begin,end,started,finished,digest,observed_core,reserved[2];
};
struct split_phase {
    uint64_t launch,readiness,reserved[6];
    struct split_core core[CAPACITY_CORES];
};
struct split_result {
    uint64_t magic,counter_hz;
    uint32_t units,warm,reps,cores;
    uint32_t capacity[CAPACITY_CORES];
    char clock_before[256],clock_after[256];
    struct split_phase samples[2][SPLIT_REPS];
};
_Static_assert(sizeof(struct split_core)==64,"private result cache line");
_Static_assert(sizeof(struct split_phase)==576,"phase protocol");
_Static_assert(sizeof(struct split_result)==4032,"split protocol");
#endif
