/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef SUNSTONE_TABLE_WALKS_RESULT_H
#define SUNSTONE_TABLE_WALKS_RESULT_H
#include <stdint.h>
#define TABLE_WALKS_MAGIC "SSTWALK1"
#define TABLE_WALKS_ITERATIONS 1024u
#define TABLE_WALKS_SAMPLES 6u /* one warm-up and five measured blocks */
struct table_walk_sample {
    uint64_t cycles, ticks, dtlb_walk, itlb_walk, bus_access, digest;
};
struct table_walk_result {
    char magic[8];
    uint64_t tcr, midr, core, iterations, counter_hz;
    char clock_before[256], clock_after[256];
    struct table_walk_sample sample[TABLE_WALKS_SAMPLES];
};
void table_walk_loop(struct table_walk_sample *, uint64_t iterations);
#endif
