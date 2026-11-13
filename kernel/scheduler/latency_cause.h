/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SS_LATENCY_CAUSE_H
#define SS_LATENCY_CAUSE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SS_LATENCY_CAUSE_MAGIC UINT64_C(0x5353434155534531)
#define SS_LATENCY_CAUSE_VERSION 3u
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
/* DynamIQ Shared Unit state that each core reads at EL3 during boot, read
 * only: CLUSTERIDR_EL1 (DSU revision), CLUSTERCFR_EL1, CLUSTERPARTCR_EL1 (L3
 * partition control), CLUSTERTHREADSID_EL1 (this core's scheme ID) and
 * ACTLR_EL3. captured is 1 once the core has stored the five values. */
struct ss_latency_cause_dsu {
    uint64_t captured, clusteridr, clustercfr, clusterpartcr, clusterthreadsid, actlr_el3;
};
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
_Static_assert(sizeof(struct ss_latency_cause_report) == 16816u, "cause report wire layout");

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
#endif
#endif
