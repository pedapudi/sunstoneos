/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SS_SCHED_LATENCY_TRACE_H
#define SS_SCHED_LATENCY_TRACE_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

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
#ifndef SS_SCHED_TRACE_HISTORY
#error "The diagnostic must declare SS_SCHED_TRACE_HISTORY"
#endif
_Static_assert(SS_SCHED_TRACE_HISTORY > 0, "diagnostic event history must retain an entry");
#define SS_SCHED_TRACE_CORES 8u
#define SS_SCHED_TRACE_RECORDS (2u * SS_OWNER_BATCH)
#define SS_SCHED_TRACE_MAGIC UINT64_C(0x5353534348545231)
#define SS_SCHED_TRACE_VERSION 5u
#define SS_SCHED_RECORD_FIFO_BUSY (UINT64_C(1) << 63)
/* Version 5 ticks_capacity: low 32 bits are the declared capacity, bit 63
 * confirms build metadata, and bit 62 identifies restore-span instrumentation.
 * Bits 32..61 are reserved. Older version-5 headers have unknown metadata.
 * See docs/designs/scheduler-latency-measurement.md, startup boundaries. */
#define SS_SCHED_TRACE_CONFIG_KNOWN (UINT64_C(1) << 63)
#define SS_SCHED_TRACE_RESTORE_SPANS (UINT64_C(1) << 62)
_Static_assert(SS_SCHED_TRACE_TICKS <= UINT32_MAX, "tick capacity must not overlap build metadata");

/* All timestamps use CNTVCT. A command retains the timestamps after masking,
 * reference, publish, before/after drain, after slice refresh and before unmask.
 * Flags: bit 0 self, bit 1 ISR, bit 2 running scheduler, bit 3 deferred,
 * bit 4 local drain, bit 5 handle admission failed. Version 5 bit 6 marks a
 * direct self-suspend: stamps 0..2 cover unmasked reference/publication;
 * stamps 3..6 cover masked local mark, saved-frame entry and selection. */
struct ss_sched_trace_command {
    uint64_t command, slot, flags, stamp[7];
};
/* Record timestamps: before FIFO acquisition, acquired, before unlock, after
 * unlock/event, after reference release/final event. Phase is the control
 * transfer phase observed under the FIFO. Version 5 busy attempts use only
 * SS_SCHED_RECORD_FIFO_BUSY, with try and park endpoints in stamps 1 and 2;
 * stamps 3 and 4 then repeat stamp 2. */
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
    SS_SCHED_WAIT_TICKETS = 8u,
    SS_SCHED_WAIT_ACTUAL_FIFO = 16u
};
/* lock 0 is the global task lock; 1 + slot is an object task lock. A coherent
 * arrival holder is authoritative only when SNAPSHOT and HOLDER are set.
 * COMPLETED means its release is matched by generation. A missing match must
 * remain unclassified; another predecessor may have run before acquisition. */
struct ss_sched_trace_wait {
    uint64_t start, end, site, lock, serving, next, flags;
    uint64_t holder_core, holder_start, holder_end, holder_site, holder_generation;
    /* Version 2: actual global task FIFO execution and LDADDA reservation.
     * The early serving/next snapshot above remains independent. Zero fields
     * without ACTUAL_FIFO mean that this acquisition was not sampled. */
    uint64_t fifo_start, fifo_end, fifo_serving, fifo_ticket;
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
enum ss_sched_trace_wake_flag {
    SS_SCHED_WAKE_IRQ = 1u, SS_SCHED_WAKE_SWITCH = 2u,
    SS_SCHED_WAKE_LINKED = 4u, SS_SCHED_WAKE_MULTIPLE = 8u,
    SS_SCHED_WAKE_UNMATCHED = 16u, SS_SCHED_WAKE_HISTORY_LOST = 32u,
    SS_SCHED_WAKE_SNAPSHOT_INCOMPLETE = 64u
};
/* An IRQ here is an ordinary owner-exit call after the PMR reentry guard.
 * Stamps: start, eligibility complete, API mask set, work done, mask restored.
 * Fast result 3 has no service; its last four stamps equal guard completion. */
struct ss_sched_trace_irq_chain {
    uint64_t stamp[5], service[5], passes, result, sequence, previous_end;
};
struct ss_sched_trace_switch {
    uint64_t begin, end, selected_slot, sequence, previous_end, irq_sequence;
};
struct ss_sched_trace_wake {
    uint64_t ready, running, flags, running_slot;
    struct ss_sched_trace_irq_chain irq;
    struct ss_sched_trace_switch selection;
};
struct ss_sched_trace_summary {
    uint64_t count, sum, maximum, over_us[5]; /* 10, 25, 50, 80, 100 us. */
};
struct ss_sched_trace_wake_core {
    struct ss_sched_trace_wake maximum;
    /* Busy samples only. Classes: linked fast, linked slow, unmatched.
     * Slow includes owner results 0 and 1 when an actual selection matches;
     * the port may switch on a yield request that predates the C return.
     * Intervals: whole wake, owner-exit start to running. The latter includes
     * work before ready when readiness occurs during owner exit. Unmatched samples
     * contribute only whole-wake durations, never an attributed tail. */
    struct ss_sched_trace_summary summary[3][2];
};
struct ss_sched_trace_wake_history {
    /* Latest completed ordinary exit no later than running. Trailing exits
     * follow the selecting IRQ through running. Overwritten counts are totals
     * at observation; future_irqs counts retained exits excluded by running. A snapshot-hole
     * flag means a single read overlaps replacement; no retry is attempted. */
    uint64_t latest_start, latest_end, latest_work, latest_result, latest_sequence;
    uint64_t trailing_count, trailing_work, irq_overwritten, switch_overwritten, future_irqs;
};
enum ss_sched_trace_window_flag {
    SS_SCHED_WINDOW_IRQ_PREFIX_LOST = 1u,
    SS_SCHED_WINDOW_SWITCH_PREFIX_LOST = 2u,
    SS_SCHED_WINDOW_INCOMPLETE = 4u
};
/* CPU0 retains complete C dispatches, including exits skipped by the PMR
 * guard. descriptor packs INTID in bits 0..15 and saved SPSR in bits 16..47.
 * sequence is the ordinary owner-exit sequence, or zero for a guarded exit. */
struct ss_sched_trace_window_irq {
    uint64_t sequence, descriptor, begin, end, pc, owner_begin, owner_end;
};
struct ss_sched_trace_window_switch {
    uint64_t begin, end, irq_sequence, selected_slot;
};
struct ss_sched_trace_cpu0_window {
    uint64_t irq_count, switch_count, flags, capacity;
    struct ss_sched_trace_window_irq irq[SS_SCHED_TRACE_HISTORY];
    struct ss_sched_trace_window_switch selection[SS_SCHED_TRACE_HISTORY];
};
_Static_assert(sizeof(struct ss_sched_trace_cpu0_window) ==
    (4u + 11u * SS_SCHED_TRACE_HISTORY) * sizeof(uint64_t), "version 5 window layout");
_Static_assert(sizeof(struct ss_sched_trace_wake_core) == 72 * sizeof(uint64_t),
               "version 3 wake record layout");
struct ss_sched_trace_report {
    uint64_t magic, version, waits_capacity, ticks_capacity; /* V5 capacity includes the metadata above. */
    struct ss_sched_trace_core core[SS_SCHED_TRACE_CORES];
    /* Version 3: core0 before xSchedulerRunning, other observed port starts;
     * first task restore just before final X0/X1 load and ERET. These are
     * boundary markers, not an upper bound on completed unmasking. */
    uint64_t startup[SS_SCHED_TRACE_CORES][2];
    uint64_t tick_count, tick_dropped;
    struct ss_sched_trace_tick tick[SS_SCHED_TRACE_TICKS];
    struct ss_sched_trace_wake_core wake[SS_SCHED_TRACE_CORES];
    /* Version 4 preserves the v3 prefix. Histories rotate within declared
     * capacity; counters expose replacement, while a per-wake loss flag means
     * a record needed to reconstruct that wake was no longer retained. */
    uint64_t history_capacity;
    struct ss_sched_trace_wake_history history[SS_SCHED_TRACE_CORES];
    /* Version 5 appends the CPU0 histories at its longest busy wake. */
    struct ss_sched_trace_cpu0_window cpu0_window;
};
_Static_assert(offsetof(struct ss_sched_trace_report, wake) ==
    offsetof(struct ss_sched_trace_report, tick) + sizeof(((struct ss_sched_trace_report *)0)->tick),
    "version 3 appends wake records after the version 2 fields");

/* Private shadow metadata is atomic because arriving CPUs read it without the
 * lock. The real lock serializes writers. Readers make one generation check,
 * never spin, and leave an inconsistent snapshot explicitly unknown. */
struct ss_sched_trace_lock {
    _Atomic uint64_t generation, core_plus_one, acquired, site;
    _Atomic uint64_t completed_generation, completed_end;
};
static inline uint64_t ss_sched_trace_now(void)
{
#ifdef SS_SCHED_TRACE_HOST_TEST
    extern uint64_t ss_sched_trace_test_now(void);
    return ss_sched_trace_test_now();
#else
    uint64_t value;
    __asm__ volatile("isb\n mrs %0, cntvct_el0" : "=r"(value) :: "memory");
    return value;
#endif
}
void ss_sched_trace_freeze(unsigned);
void ss_sched_trace_snapshot(struct ss_sched_trace_report *);
void ss_sched_trace_startup(unsigned, unsigned);
void ss_sched_trace_command(unsigned, const struct ss_sched_trace_command *);
void ss_sched_trace_direct_suspend_begin(unsigned, const struct ss_sched_trace_command *);
void ss_sched_trace_direct_suspend_mark(unsigned);
void ss_sched_trace_drain_begin(unsigned);
void ss_sched_trace_drain_end(unsigned);
void ss_sched_trace_record(unsigned, const struct ss_sched_trace_record *);
/* Only an accepted entry may emit guard, stage, fast or end markers. Nested
 * owner entries leave the active parent intact; dispatch history is separate. */
bool ss_sched_trace_irq_begin(unsigned, uint64_t);
void ss_sched_trace_irq_guard(unsigned);
void ss_sched_trace_irq_fast(unsigned);
void ss_sched_trace_irq_stamp(unsigned, unsigned);
void ss_sched_trace_irq_end(unsigned, unsigned);
uint64_t ss_sched_trace_dispatch_begin(unsigned, unsigned, uint64_t, uint64_t);
void ss_sched_trace_dispatch_end(unsigned, uint64_t);
void ss_sched_trace_service(unsigned, unsigned, uint64_t, uint64_t);
void ss_sched_trace_service_pass(unsigned);
void ss_sched_trace_tick_begin(void);
void ss_sched_trace_tick_end(void);
void ss_sched_trace_fifo_sample(unsigned, uint64_t, uint64_t, uint32_t);
void ss_sched_trace_switch_begin(unsigned);
void ss_sched_trace_switch_end(unsigned, unsigned);
void ss_sched_trace_wake_sample(unsigned, uint64_t, uint64_t, unsigned);
struct ss_sched_trace_wait ss_sched_trace_lock_begin(const struct ss_sched_trace_lock *, unsigned, uintptr_t,
                                                    uint64_t, uint64_t, unsigned);
void ss_sched_trace_lock_acquired(struct ss_sched_trace_lock *, unsigned, struct ss_sched_trace_wait *);
void ss_sched_trace_lock_release(struct ss_sched_trace_lock *);
#endif
#endif
