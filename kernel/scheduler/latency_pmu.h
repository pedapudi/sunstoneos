/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SS_SCHED_LATENCY_PMU_H
#define SS_SCHED_LATENCY_PMU_H
#include <stdint.h>
#include <stddef.h>

#define SS_SCHED_PMU_MAGIC UINT64_C(0x5353504d55303031)
#define SS_SCHED_PMU_VERSION 1u
#define SS_SCHED_PMU_CORES 8u
#define SS_SCHED_PMU_BANKS 2u
#define SS_SCHED_PMU_EVENTS 6u

enum ss_sched_pmu_kind {
    SS_SCHED_PMU_SELECT_SELF,
    SS_SCHED_PMU_SELECT_OTHER,
    SS_SCHED_PMU_CONTROL,
    SS_SCHED_PMU_TICK,
    SS_SCHED_PMU_BLANK,
    SS_SCHED_PMU_KINDS
};
enum ss_sched_pmu_status {
    SS_SCHED_PMU_OK,
    SS_SCHED_PMU_UNARMED,
    SS_SCHED_PMU_NOT_EL1,
    SS_SCHED_PMU_WRONG_CORE,
    SS_SCHED_PMU_NESTED,
    SS_SCHED_PMU_NOT_ACTIVE,
    SS_SCHED_PMU_WRONG_COOKIE,
    SS_SCHED_PMU_NO_COUNTERS,
    SS_SCHED_PMU_CONFIG_MISMATCH,
    SS_SCHED_PMU_NOT_PREPARED,
    SS_SCHED_PMU_FROZEN,
    SS_SCHED_PMU_OVERFLOW
};
enum ss_sched_pmu_flag {
    SS_SCHED_PMU_SELF = 1u,
    SS_SCHED_PMU_RAW_COUNTS = 2u,
    /* Rejections return synchronously without writing during an outer window.
     * A zero nested_skips field does not establish that no call was rejected. */
    SS_SCHED_PMU_NESTED_COUNT_UNAVAILABLE = 4u,
    SS_SCHED_PMU_SNAPSHOT_INCOMPLETE = 8u
};

/* Every report word is an unsigned little-endian 64-bit integer. Event index
 * maps to the row's bank: {03,01,17,2A,34,35} or {03,01,17,24,34,35}. A clear
 * supported bit means unavailable, irrespective of the corresponding zero. */
struct ss_sched_pmu_row {
    uint64_t count, nested_skips, status;
    uint64_t begin_tick, end_tick, cycles, event[SS_SCHED_PMU_EVENTS];
    uint64_t supported, overflow, slot, flags, cookie;
};
struct ss_sched_pmu_core {
    uint64_t status, midr, counter_count, pmceid0, pmceid1;
    uint64_t supported[SS_SCHED_PMU_BANKS];
    uint64_t configured_banks, attempts, completed, reserved[2];
    struct ss_sched_pmu_row row[SS_SCHED_PMU_KINDS][SS_SCHED_PMU_BANKS];
};
struct ss_sched_pmu_report {
    uint64_t magic, version, bytes, cores, kinds, banks, events, flags;
    struct ss_sched_pmu_core core[SS_SCHED_PMU_CORES];
};
_Static_assert(sizeof(struct ss_sched_pmu_row) == 17u * 8u, "PMU row wire layout");
_Static_assert(sizeof(struct ss_sched_pmu_core) == 182u * 8u, "PMU core wire layout");
_Static_assert(sizeof(struct ss_sched_pmu_report) == 1464u * 8u, "PMU report wire layout");

#ifdef SUNSTONE_SCHED_PMU
#if !defined(SUNSTONE_LATENCY) || !defined(SUNSTONE_SCHED_LATENCY_TRACE)
#error "Scheduler PMU capture requires latency tracing"
#endif
#if defined(SUNSTONE_LATENCY_PMU) || defined(SUNSTONE_PMU_ACCOUNT) || defined(SUNSTONE_PMU_RESULT)
#error "Scheduler PMU capture requires exclusive counter ownership"
#endif
/* Arm, per-core freeze and snapshot use ordinary memory and may run at EL0.
 * Freeze validates the CPU from its current task tag and acknowledges that
 * future local captures are refused. Snapshot requires all acknowledgments.
 * Prepare runs only
 * on the named EL1 CPU, before existing timing markers. Begin returns a positive
 * cookie, or the negated status above; only a positive cookie may be ended.
 * The end leaf disables counting before its first spill or recorder store. */
void ss_sched_pmu_arm(void);
int ss_sched_pmu_prepare(unsigned core, unsigned kind);
int64_t ss_sched_pmu_begin(unsigned core, unsigned kind, unsigned slot, unsigned flags);
int ss_sched_pmu_end(unsigned core, int64_t cookie);
/* Scope callers ignore the begin result and pass the address of an uninitialized
 * local byte at both boundaries. Its contents are never accessed. A rejected
 * nested scope cannot finish its parent's capture. This avoids live-cookie
 * spills after enable in large callers. */
int64_t ss_sched_pmu_begin_scope(unsigned core,unsigned kind,unsigned slot,unsigned flags,void *scope);
int ss_sched_pmu_end_scope(unsigned core,void *scope);
int ss_sched_pmu_freeze(unsigned core);
void ss_sched_pmu_snapshot(struct ss_sched_pmu_report *out);
#endif
#endif
