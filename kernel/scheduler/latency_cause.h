/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SS_LATENCY_CAUSE_H
#define SS_LATENCY_CAUSE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SS_LATENCY_CAUSE_MAGIC UINT64_C(0x5353434155534531)
#define SS_LATENCY_CAUSE_VERSION 4u
#define SS_LATENCY_CAUSE_CORES 8u
#define SS_LATENCY_CAUSE_PHASES 2u
#define SS_LATENCY_CAUSE_SAMPLES 64u
/* Event counters 0-4: L1I refill, L1D refill, L2D refill, attributable L3D
 * refill and backend stall cycles. The counters are 32 bits wide; every
 * recorded value is a modular difference and the decoder needs no overflow. */
#define SS_LATENCY_CAUSE_EVENTS 5u
#define SS_LATENCY_CAUSE_EVENT_TYPES 0x01u, 0x03u, 0x17u, 0x2au, 0x24u

enum ss_latency_cause_case {
    SS_CAUSE_BLANK, SS_CAUSE_ISB, SS_CAUSE_DSB_ISH, SS_CAUSE_DSB_SY,
    SS_CAUSE_STORE, SS_CAUSE_STORE_ISH, SS_CAUSE_STORE_SY,
    SS_CAUSE_SGI, SS_CAUSE_SGI_ISH, SS_CAUSE_SGI_SY, SS_CAUSE_PMR,
    SS_CAUSE_SGI_SYNC, SS_CAUSE_SGI_SYNC_ISH, SS_CAUSE_SGI_SYNC_SY, SS_CAUSE_PMR_SY,
    SS_CAUSE_CASES
};
enum ss_latency_cause_status {
    SS_CAUSE_OK, SS_CAUSE_UNARMED, SS_CAUSE_NOT_EL1, SS_CAUSE_WRONG_CORE,
    SS_CAUSE_CONFIG, SS_CAUSE_FROZEN, SS_CAUSE_NESTED
};
enum ss_latency_cause_flag {
    SS_CAUSE_RAW = 1u, SS_CAUSE_SNAPSHOT_INCOMPLETE = 2u,
    SS_CAUSE_COMPONENT_OBSERVER_INCLUDED = 4u,
    SS_CAUSE_BENCH_IRQ_MASKED = 8u
};
/* Every wire field is a little-endian uint64_t. Bench samples are raw cycles;
 * no blank subtraction occurs in the image. A case's unused sample tail is zero.
 * The SGI cases address CPU1 with the production reschedule SGI value. */
struct ss_latency_cause_core {
    uint64_t status, midr, mpidr, pmcr, userenr, filter, enabled, overflow;
    uint64_t irq_count, ready, frozen, icc_ctlr, pmr, sgi_irq_count;
    uint64_t cycle_begin, tick_begin, cycle_end, tick_end;
};
struct ss_latency_cause_sgi {
    uint64_t count, total_cycles, max_cycles, interrupted;
};
struct ss_latency_cause_target {
    uint64_t release_count, release_total, release_max, release_sgi_total;
    uint64_t release_sgi_max, release_sends;
    uint64_t state_count, state_total, state_max, state_sgi_total;
    uint64_t state_sgi_max, state_sends;
    uint64_t mask_tail_total, mask_tail_max;
};
struct ss_latency_cause_tick_target {
    uint64_t release_begin, release_end, release_sgi_cycles, release_sends;
    uint64_t state_begin, state_end, state_sgi_cycles, state_sends;
    uint64_t deadline_state, deadline, slice_deadline, due;
    uint64_t mask_tail_begin, mask_tail_end;
};
struct ss_latency_cause_tick {
    uint64_t count, begin_tick, end_tick, begin_cycles, end_cycles;
    uint64_t release_mask, state_mask, interrupted;
    struct ss_latency_cause_tick_target target[SS_LATENCY_CAUSE_CORES];
};
struct ss_latency_cause_bench {
    uint64_t requested, done, status;
    uint64_t sgi_sends, sgi_observed, delivery_wait_ticks, pending;
    uint64_t observer_count, observer_min, observer_median, observer_max;
    uint64_t count[SS_CAUSE_CASES];
    uint64_t cycles[SS_CAUSE_CASES][SS_LATENCY_CAUSE_SAMPLES];
};
/* Event differences per segment. The retained tick's rows match its cycle
 * stamps. Phase rows sum complete ticks only (all eight releases and scans);
 * outside is the tick minus its releases and scans. */
struct ss_latency_cause_events {
    uint64_t types[SS_LATENCY_CAUSE_EVENTS], pmceid0[SS_LATENCY_CAUSE_CORES];
    uint64_t tick[SS_LATENCY_CAUSE_EVENTS];
    uint64_t release[SS_LATENCY_CAUSE_CORES][SS_LATENCY_CAUSE_EVENTS];
    uint64_t scan[SS_LATENCY_CAUSE_CORES][SS_LATENCY_CAUSE_EVENTS];
    uint64_t phase_ticks[SS_LATENCY_CAUSE_PHASES], phase_cycles[SS_LATENCY_CAUSE_PHASES];
    uint64_t phase_outside_cycles[SS_LATENCY_CAUSE_PHASES];
    uint64_t phase_events[SS_LATENCY_CAUSE_PHASES][SS_LATENCY_CAUSE_EVENTS];
    uint64_t phase_outside[SS_LATENCY_CAUSE_PHASES][SS_LATENCY_CAUSE_EVENTS];
    uint64_t phase_release_cycles[SS_LATENCY_CAUSE_PHASES][SS_LATENCY_CAUSE_CORES];
    uint64_t phase_scan_cycles[SS_LATENCY_CAUSE_PHASES][SS_LATENCY_CAUSE_CORES];
    uint64_t phase_release[SS_LATENCY_CAUSE_PHASES][SS_LATENCY_CAUSE_CORES][SS_LATENCY_CAUSE_EVENTS];
    uint64_t phase_scan[SS_LATENCY_CAUSE_PHASES][SS_LATENCY_CAUSE_CORES][SS_LATENCY_CAUSE_EVENTS];
};
/* DynamIQ Shared Unit state that each core reads at EL3 during boot, before
 * any write: CLUSTERIDR_EL1 (DSU revision), CLUSTERCFR_EL1, CLUSTERPARTCR_EL1
 * (L3 partition control), CLUSTERTHREADSID_EL1 (this core's scheme ID),
 * ACTLR_EL3, CLUSTERPWRSTAT_EL1 (L3 portions powered), CLUSTERACPSID_EL1,
 * CLUSTERSTASHSID_EL1 and CLUSTERTHREADSIDOVR_EL1. captured is 1 once the core
 * has stored them. A partitioned build (SUNSTONE_LATENCY_L3_PARTITION) then
 * writes: threadsid_set is the scheme ID this core wrote and threadsid_after
 * its readback; partition is the CLUSTERPARTCR_EL1 value CPU0 wrote, and CPU0's
 * partcr_after its readback. All four are 0 in other builds. Any entry to EL3
 * clears both registers before the image stops (continue.S). */
struct ss_latency_cause_dsu {
    uint64_t captured, clusteridr, clustercfr, clusterpartcr, clusterthreadsid, actlr_el3;
    uint64_t clusterpwrstat, clusteracpsid, clusterstashsid, clusterthreadsidovr;
    uint64_t threadsid_set, threadsid_after, partition, partcr_after;
};
/* Way groups 1-3 private to scheme 0 (bits 1-3), group 0 private to scheme 1
 * (bit 4): DSU TRM 100453_0401 section 11.12. Cores outside the stream mask
 * use scheme 1. */
#define SS_LATENCY_CAUSE_L3_PARTCR UINT64_C(0x1e)
struct ss_latency_cause_report {
    uint64_t magic, version, bytes, cores, phases, cases, samples, flags;
    struct ss_latency_cause_core core[SS_LATENCY_CAUSE_CORES];
    struct ss_latency_cause_sgi sgi[SS_LATENCY_CAUSE_CORES][SS_LATENCY_CAUSE_CORES];
    struct ss_latency_cause_target target[SS_LATENCY_CAUSE_PHASES][SS_LATENCY_CAUSE_CORES];
    struct ss_latency_cause_tick tick;
    struct ss_latency_cause_bench bench;
    struct ss_latency_cause_events events;
    struct ss_latency_cause_dsu dsu[SS_LATENCY_CAUSE_CORES];
};
_Static_assert(sizeof(struct ss_latency_cause_events) == 2528u, "cause event section layout");
_Static_assert(sizeof(struct ss_latency_cause_report) == 17328u, "cause report wire layout");

/* Wake segments (measurement build only, SUNSTONE_LATENCY_WAKE_SEGMENTS): the
 * cycles and the five cause events of each segment of a busy wake on the
 * woken core, from the C entry of the interrupt that leads to the selection to
 * the point where the woken task reads its running time. Points, in order:
 * 0 interrupt C entry (latency wrapper), 1 wrapper return, 2 owner exit start,
 * 3 owner work start, 4 work done, 5 mask restored, 6 selection begin,
 * 7 selection end, 8 running. Segment s runs from point s to point s + 1. The
 * service parts (control drain, incoming, pull, expiry, slice) are summed over
 * the passes inside segment 3.
 *
 * Each wake has two start stamps in generic counter ticks. Its deadline is
 * the counter value its wake was due at: the compare value of the timer
 * interrupt that released it, CPU0's for a core CPU0's tick releases. Its
 * release is the stamp at which the task became ready: on CPU0's tick for a
 * released core, and the deadline itself for a core that times itself. A
 * wake's latency runs from its deadline to running; the time from the
 * deadline to point 0 is kept in ticks only, since the deadline is not a
 * cycle count on the woken core.
 *
 * A wake is linked when one outermost interrupt passed every point in order
 * and switched after the release; on CPUs 1-7 that interrupt must start
 * after the release, and on CPU0, which releases its own task inside its
 * tick, the release must fall inside the chain. An interrupt that arrives
 * after the switch and before running starts a new chain but keeps the
 * completed one, which then links with the interrupt's time in its last
 * segment. Any other wake is counted unlinked. The last segment ends where the
 * woken task marks its running stamp (ss_wake_seg_mark), before its own
 * bookkeeping. Cells are filed by the band setting at running (1 when the
 * build has no band switch). CPU0's busy complete ticks are filed by the band
 * setting at the tick's start, and the first and second busy complete ticks
 * are kept whole. A complete tick released every CPU of the release mask and
 * read the deadlines of every CPU of the scan mask (header words 8 and 9).
 * Cores count their local timer arms (configSS_LOCAL_TIMERS) with each arm's
 * cycles net of an empty bracket.
 *
 * Version 3 adds the deadline to each sample, keeps each cell's three largest
 * and its smallest cycles of every segment and part, and keeps the three
 * largest wakes of each setting, measured from the deadline.
 *
 * Version 4 has the layout of version 3. The woken task copies its linked
 * chain with interrupts open, and copies again when an interrupt on its core
 * started a chain during the copy; first_state counts the wakes torn in every
 * copy. SS_WAKE_SEG_COPIES bounds the copies. One copy is a chain_state of 752
 * bytes (latency_cause.c), 94 ns at the 8.0 GB/s that apps/memcpy-bench
 * measured for aligned copies in the caches on an A55 at 1794 MHz
 * (docs/designs/code-generation-flags.md). CPU0 takes the most interrupts:
 * the tick and, under the NPU, up to one completion per job, 177,768 jobs/s
 * (docs/status/latency.md). With interrupts arriving at random at that rate, a
 * copy is torn with a probability of about 0.02 and four copies in a row with
 * about 2e-7, or about one wake in 5 million. */
#define SS_WAKE_SEG_MAGIC UINT64_C(0x5353574b53454731)
#define SS_WAKE_SEG_FOOTER_MAGIC UINT64_C(0x5353574b53454631)
#define SS_WAKE_SEG_VERSION 4u
#define SS_WAKE_SEG_COPIES 4u
#define SS_WAKE_POINTS 9u
#define SS_WAKE_SEGMENTS (SS_WAKE_POINTS - 1u)
#define SS_WAKE_PARTS 5u
#define SS_WAKE_SPANS (SS_WAKE_SEGMENTS + SS_WAKE_PARTS)
#define SS_WAKE_WORST 3u
struct ss_wake_seg_values {
    uint64_t cycles, events[SS_LATENCY_CAUSE_EVENTS];
};
/* index: the busy sample number in bits 0-31, bit 32 when an interrupt came
 * between the switch and running (its time is in the last segment), bit 33
 * when the chain's interrupt nested another. */
struct ss_wake_seg_sample {
    uint64_t deadline, release, entry_tick, running, index;
    struct ss_wake_seg_values segment[SS_WAKE_SEGMENTS], part[SS_WAKE_PARTS];
};
/* pre_ticks sums deadline to point 0, whole_ticks deadline to running and
 * release_ticks release to running. top[span] holds the span's three largest
 * cycle counts, largest first, and least its smallest; spans are the
 * segments, then the parts. */
struct ss_wake_seg_cell {
    uint64_t count, pre_ticks, whole_ticks, release_ticks;
    struct ss_wake_seg_values segment[SS_WAKE_SEGMENTS], part[SS_WAKE_PARTS];
    uint64_t top[SS_WAKE_SPANS][SS_WAKE_WORST], least[SS_WAKE_SPANS];
};
/* first: busy sample #0; worst[setting]: the three largest wakes after #0,
 * from the deadline, largest first. */
struct ss_wake_seg_core {
    /* linked counts every linked wake, #0 included; interrupted and nested
     * count linked wakes with those flags. */
    uint64_t linked, unlinked, nested, interrupted;
    /* The first busy wake's chain at running: bit 16 seen, bit 8 linked, bit 9
     * linked through the held chain, bit 10 torn, bits 0-7 the next point the
     * current chain expected. Bits 32-63 count the busy wakes, #0 included,
     * whose chain was torn in every copy (SS_WAKE_SEG_COPIES); such a wake is
     * neither linked nor unlinked and files nothing. arms counts the timer register writes this core made while
     * measuring; arm_cycles sums their cycles and arm_max is the largest. */
    uint64_t first_state, arms, arm_cycles, arm_max;
    struct ss_wake_seg_cell cell[2];
    struct ss_wake_seg_sample first, worst[2][SS_WAKE_WORST];
};
struct ss_wake_seg_tick {
    uint64_t count, cycles, outside_cycles, crossed;
    uint64_t events[SS_LATENCY_CAUSE_EVENTS], outside_events[SS_LATENCY_CAUSE_EVENTS];
    uint64_t release_cycles[SS_LATENCY_CAUSE_CORES];
    uint64_t release_events[SS_LATENCY_CAUSE_CORES][SS_LATENCY_CAUSE_EVENTS];
};
struct ss_wake_seg_report {
    uint64_t magic, version, bytes, cores, points, parts, switch_shift, complete;
    uint64_t release_mask, scan_mask, reserved[6];
    struct ss_wake_seg_core core[SS_LATENCY_CAUSE_CORES];
    /* tick[setting]: busy complete ticks; first, second: busy complete ticks 1 and 2. */
    struct ss_wake_seg_tick tick[2], first, second;
};
_Static_assert(sizeof(struct ss_wake_seg_sample) == 83u * 8u, "wake segment sample layout");
_Static_assert(sizeof(struct ss_wake_seg_cell) == 134u * 8u, "wake segment cell layout");
_Static_assert(sizeof(struct ss_wake_seg_core) == 857u * 8u, "wake segment core layout");
_Static_assert(sizeof(struct ss_wake_seg_report) == 128u + 8u * 857u * 8u + 4u * 62u * 8u, "wake segment report layout");

#ifdef SUNSTONE_LATENCY_CAUSE
#if !defined(SUNSTONE_LATENCY) || !defined(SUNSTONE_SCHED_LATENCY_TRACE)
#error "Latency cause capture requires the latency trace"
#endif
#if defined(SUNSTONE_SCHED_PMU) || defined(SUNSTONE_LATENCY_PMU) || defined(SUNSTONE_PMU_ACCOUNT) || defined(SUNSTONE_PMU_RESULT)
#error "Latency cause capture owns the cycle counter exclusively"
#endif
/* EL0-safe ordinary-memory entry points. The caller does not read PMCCNTR until
 * its named core is ready. Each pinned task freezes its own core before the
 * final snapshot, which refuses any missing acknowledgment. */
void ss_latency_cause_arm(void);
/* Each core writes its own entry at EL3 after it enables EL3 translation, so
 * the store goes through the write-back Nonsecure view that EL1 reads. The
 * snapshot copies the entries into the report. */
extern struct ss_latency_cause_dsu ss_latency_cause_dsu_boot[SS_LATENCY_CAUSE_CORES];
bool ss_latency_cause_core_ready(unsigned core);
uint64_t ss_latency_cause_irq_count(unsigned core);
void ss_latency_cause_bench_request(void);
bool ss_latency_cause_bench_done(void);
void ss_latency_cause_freeze(unsigned core);
bool ss_latency_cause_snapshot(void *out, size_t bytes);
/* IRQ wrapper and release hooks use EL1 on the actual CPU. Phase0 is idle and
 * phase1 busy. A release outside those phases is retained only in tick detail.
 * New component hook bookkeeping is included and flagged; microbench assembly
 * holds its timing samples in registers until the measured endpoint. */
void ss_latency_cause_irq_begin(unsigned core, unsigned interrupt);
void ss_latency_cause_irq_end(unsigned core);
void ss_latency_cause_tick_begin(void);
void ss_latency_cause_tick_end(void);
void ss_latency_cause_release_begin(unsigned target, unsigned phase);
void ss_latency_cause_release_end(unsigned target);
void ss_latency_cause_state_begin(unsigned target);
void ss_latency_cause_state_end(unsigned target, unsigned state, uint64_t deadline,
                                uint64_t slice_deadline, bool due);
void ss_latency_cause_mask_tail_begin(void);
void ss_latency_cause_mask_tail_end(void);
void ss_latency_cause_sgi_begin(unsigned target);
void ss_latency_cause_sgi_end(unsigned target);
void ss_latency_cause_sgi_sent(unsigned target);
#ifdef SUNSTONE_LATENCY_WAKE_SEGMENTS
/* Points 2-7 at EL1 on the named core (point 0 and 1 come from the IRQ hooks
 * above). A part brackets one service part inside the owner work. Running is
 * called by the woken task itself, at EL0 or EL1, with its deadline, release
 * and running stamps, its busy sample index and the band setting it applied. */
void ss_wake_seg_point(unsigned core, unsigned point);
void ss_wake_seg_fast(unsigned core);
void ss_wake_seg_part_begin(unsigned core);
void ss_wake_seg_part_end(unsigned core, unsigned part);
void ss_wake_seg_mark(unsigned core);
void ss_wake_seg_running(unsigned core, uint64_t deadline, uint64_t release, uint64_t running, uint64_t index,
                         unsigned setting);
bool ss_wake_seg_snapshot(struct ss_wake_seg_report *out);
/* A local timer arm at EL1 (kernel/scheduler/freertos.c): begin takes two
 * stamps, the second closing an empty bracket; end counts the arm. */
void ss_wake_seg_arm_begin(unsigned core, uint64_t stamp[2]);
void ss_wake_seg_arm_end(unsigned core, const uint64_t stamp[2]);
#endif
/* The CPUs whose deadlines CPU0's tick reads (kernel/scheduler/freertos.c). */
unsigned ss_kernel_tick_scan_mask(void);
#endif
#include "service_cycles.h"
#endif
