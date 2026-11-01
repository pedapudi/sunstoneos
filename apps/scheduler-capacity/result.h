#ifndef SCHEDULER_CAPACITY_RESULT_H
#define SCHEDULER_CAPACITY_RESULT_H
#include "sunstone_cpu_observation.h"
#define CAPACITY_MAGIC UINT64_C(0x5353434150413031)
#define CAPACITY_WORDS 4096u
#define CAPACITY_ROUNDS 64u
#define CAPACITY_WARM 3u
#define CAPACITY_REPS 7u
struct capacity_sample { uint64_t ticks, digest; };
struct capacity_core {
    struct ss_cpu_observation observation;
    uint32_t observed_core, reserved;
    char clock_before[256], clock_after[256];
    struct capacity_sample samples[2][CAPACITY_REPS];
};
struct capacity_result {
    uint64_t magic, counter_hz;
    uint32_t words, rounds, warm, reps;
    struct capacity_core core[8];
};
uint64_t capacity_integer(uint32_t *);
uint64_t capacity_neon(uint32_t *);
#endif
