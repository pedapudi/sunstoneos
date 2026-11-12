/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SS_SCHED_LATENCY_TRACE_H
#define SS_SCHED_LATENCY_TRACE_H
#include <stdint.h>

#ifdef SUNSTONE_SCHED_LATENCY_TRACE
#ifndef SUNSTONE_LATENCY
#error "Scheduler tracing requires the latency recorder"
#endif
#include <stdatomic.h>
#include "owner.h"
#ifndef SS_SCHED_TRACE_WAITS
#error "The diagnostic must declare SS_SCHED_TRACE_WAITS"
#endif
#ifndef SS_SCHED_TRACE_TICKS
#error "The diagnostic must declare SS_SCHED_TRACE_TICKS"
#endif
#define SS_SCHED_TRACE_CORES 8u
#define SS_SCHED_TRACE_RECORDS (2u * SS_OWNER_BATCH)
#define SS_SCHED_TRACE_MAGIC UINT64_C(0x5353534348545231)

/* All timestamps use CNTVCT. A command retains the timestamps after masking,
 * reference, publish, before/after drain, after slice refresh and before unmask.
 * Flags: bit 0 self, bit 1 ISR, bit 2 running scheduler, bit 3 deferred,
 * bit 4 local drain, bit 5 handle admission failed. */
struct ss_sched_trace_command {
    uint64_t command, slot, flags, stamp[7];
};
/* Record timestamps: before FIFO acquisition, acquired, before unlock, after
 * unlock/event, after reference release/final event. Phase is the control
 * transfer phase observed under the FIFO. */
struct ss_sched_trace_record {
    uint64_t slot, phase, stamp[5];
};
struct ss_sched_trace_drain {
    uint64_t start, end, count, dropped;
    struct ss_sched_trace_record record[SS_SCHED_TRACE_RECORDS];
};
/* IRQ stamps: entry, API mask raised, owner work complete, mask restored.
 * Service ticks accumulate control, incoming, pull, expiry and slice work. */
struct ss_sched_trace_irq {
    uint64_t stamp[4], service[5], passes, result;
};
enum ss_sched_trace_wait_flag {
    SS_SCHED_WAIT_HOLDER = 1u,
    SS_SCHED_WAIT_SNAPSHOT = 2u,
    SS_SCHED_WAIT_COMPLETED = 4u,
    SS_SCHED_WAIT_TICKETS = 8u
};
/* lock 0 is the global task lock; 1 + slot is an object task lock. A coherent
 * arrival holder is authoritative only when SNAPSHOT and HOLDER are set.
 * COMPLETED means its release is matched by generation. A missing match must
 * remain unclassified; another predecessor may have run before acquisition. */
struct ss_sched_trace_wait {
    uint64_t start, end, site, lock, serving, next, flags;
    uint64_t holder_core, holder_start, holder_end, holder_site, holder_generation;
};
struct ss_sched_trace_core {
    uint64_t command_count, command_stage_max[4], irq_count, drain_count;
    uint64_t wait_count, wait_dropped;
    struct ss_sched_trace_command command, self_suspend;
    struct ss_sched_trace_irq irq;
    struct ss_sched_trace_drain drain;
    struct ss_sched_trace_wait wait_max, wait[SS_SCHED_TRACE_WAITS];
};
struct ss_sched_trace_tick { uint64_t start, end; };
struct ss_sched_trace_report {
    uint64_t magic, version, waits_capacity, ticks_capacity;
    struct ss_sched_trace_core core[SS_SCHED_TRACE_CORES];
    /* Port scheduler-start entry and first context-restore entry. The second
     * marker precedes vector setup and restoration; this interval is a lower
     * bound on masking after scheduler start. Zero means unobserved. */
    uint64_t startup[SS_SCHED_TRACE_CORES][2];
    uint64_t tick_count, tick_dropped;
    struct ss_sched_trace_tick tick[SS_SCHED_TRACE_TICKS];
};

/* Private shadow metadata is atomic because arriving CPUs read it without the
 * lock. The real lock serializes writers. Readers make one generation check,
 * never spin, and leave an inconsistent snapshot explicitly unknown. */
struct ss_sched_trace_lock {
    _Atomic uint64_t generation, core_plus_one, acquired, site;
    _Atomic uint64_t completed_generation, completed_end;
};
static inline uint64_t ss_sched_trace_now(void)
{
    uint64_t value;
    __asm__ volatile("isb\n mrs %0, cntvct_el0" : "=r"(value) :: "memory");
    return value;
}
void ss_sched_trace_freeze(unsigned);
void ss_sched_trace_snapshot(struct ss_sched_trace_report *);
void ss_sched_trace_startup(unsigned, unsigned);
void ss_sched_trace_command(unsigned, const struct ss_sched_trace_command *);
void ss_sched_trace_drain_begin(unsigned);
void ss_sched_trace_drain_end(unsigned);
void ss_sched_trace_record(unsigned, const struct ss_sched_trace_record *);
void ss_sched_trace_irq_begin(unsigned, uint64_t);
void ss_sched_trace_irq_stamp(unsigned, unsigned);
void ss_sched_trace_irq_end(unsigned, unsigned);
void ss_sched_trace_service(unsigned, unsigned, uint64_t, uint64_t);
void ss_sched_trace_service_pass(unsigned);
void ss_sched_trace_tick_begin(void);
void ss_sched_trace_tick_end(void);
struct ss_sched_trace_wait ss_sched_trace_lock_begin(const struct ss_sched_trace_lock *, unsigned, uintptr_t,
                                                    uint64_t, uint64_t, unsigned);
void ss_sched_trace_lock_acquired(struct ss_sched_trace_lock *, unsigned, struct ss_sched_trace_wait *);
void ss_sched_trace_lock_release(struct ss_sched_trace_lock *);
#endif
#endif
