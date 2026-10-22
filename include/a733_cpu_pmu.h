/* SPDX-License-Identifier: Apache-2.0 */
/* Per-core cycle and event counts around a measurement region, at EL1.
 *
 * Only a task that runs at EL1 calls these functions, and its affinity mask
 * must hold only the core it runs on. Begin and end run on that core; the
 * region lives on the caller's stack and the record in the caller's result
 * buffer. The counts include everything that runs on the core inside the
 * region, interrupt handlers included. See docs/cpu-pmu.md. */
#ifndef A733_CPU_PMU_H
#define A733_CPU_PMU_H
#include <stdint.h>

#define A733_CPU_PMU_CORES 8u
#define A733_CPU_PMU_EVENTS 6u
#define A733_CPU_PMU_FORMAT 1u
/* Core value of a region that begin refused. */
#define A733_CPU_PMU_NO_CORE 0xffu

/* Index of each event in a record's events[], supported mask and overflow
 * mask. The event number follows each name. */
enum a733_cpu_pmu_event {
    A733_CPU_PMU_INST_RETIRED = 0,     /* 0x08 */
    A733_CPU_PMU_L1D_CACHE_REFILL = 1, /* 0x03 */
    A733_CPU_PMU_L2D_CACHE_REFILL = 2, /* 0x17 */
    A733_CPU_PMU_BUS_ACCESS = 3,       /* 0x19 */
    A733_CPU_PMU_DTLB_WALK = 4,        /* 0x34 */
    A733_CPU_PMU_ITLB_WALK = 5,        /* 0x35 */
};

enum a733_cpu_pmu_status {
    A733_CPU_PMU_OK = 0,
    A733_CPU_PMU_BAD_ARGUMENT = 1,       /* A null region or record. */
    A733_CPU_PMU_NOT_PINNED = 2,         /* The affinity mask is not exactly the current core. */
    A733_CPU_PMU_NO_COUNTERS = 3,        /* PMCR_EL0.N reads 0. */
    A733_CPU_PMU_CONFIG_MISMATCH = 4,    /* A configuration register read back another value. */
    A733_CPU_PMU_NOT_BEGUN = 5,          /* End on a region that begin refused. */
    A733_CPU_PMU_CORE_CHANGED = 6,       /* End runs on another core than begin. */
    A733_CPU_PMU_GENERATION_CHANGED = 7, /* Another region began on the core. */
    A733_CPU_PMU_DISABLED = 8,           /* PMCR_EL0.E reads 0 at end, as after a warm reset. */
};

/* Written by begin, read by end. */
struct a733_cpu_pmu_region {
    uint32_t generation;
    uint8_t core;
    uint8_t reserved[3];
    uint64_t cycles;
    uint64_t ticks;
};

/* One 64-byte little-endian record. On a refusal every field other than
 * format, core and status is zero; core is the core that ran end, or
 * A733_CPU_PMU_NO_CORE when end refused its arguments. events[i] is the count
 * of event i when bit i of supported is set and bit i of overflow is clear,
 * and zero otherwise. An overflowed event occurred at least 2^32 times. */
struct a733_cpu_pmu_record {
    uint8_t format;
    uint8_t core;
    uint8_t counters;  /* Event counters used: min(PMCR_EL0.N, 6). */
    uint8_t supported; /* Bit i: event i is implemented (PMCEID0/1_EL0) and has a counter. */
    uint8_t overflow;  /* Bit i: event i overflowed; its count is not reported. */
    uint8_t status;    /* enum a733_cpu_pmu_status */
    uint16_t reserved0;
    uint32_t midr;     /* MIDR_EL1 */
    uint32_t cntfrq;   /* CNTFRQ_EL0 */
    uint64_t cycles;   /* PMCCNTR_EL0 difference */
    uint64_t ticks;    /* CNTPCT_EL0 difference */
    uint32_t events[A733_CPU_PMU_EVENTS];
    uint64_t reserved1;
};

_Static_assert(sizeof(struct a733_cpu_pmu_region) <= 32, "region size");
_Static_assert(sizeof(struct a733_cpu_pmu_record) == 64, "record size");

/* Configure the core's PMU if needed, zero the event counters and store the
 * starting counts. Returns enum a733_cpu_pmu_status. */
int a733_cpu_pmu_begin(struct a733_cpu_pmu_region *region);
/* Read the counts on the same core and write one record, also on a refusal
 * when record is not null. Returns the record's status. */
int a733_cpu_pmu_end(const struct a733_cpu_pmu_region *region, struct a733_cpu_pmu_record *record);

#endif
