/* SPDX-License-Identifier: Apache-2.0 */
/* Hard real-time control interrupts above the kernel API mask (SUNSTONE_CONTROL_IRQ, measurement).
 * A core-local timer interrupt runs a handler that calls no kernel API, measures its own latency and
 * period jitter, and hands samples to a task through a lock-free ring. */
#ifndef SUNSTONE_CONTROL_H
#define SUNSTONE_CONTROL_H
#include <stdint.h>
#define SS_CONTROL_CORES 8u
/* path 0: the control interrupt handler; path 1: a task woken by an interrupt at the API floor through the kernel */
struct ss_control_stats {
    uint32_t path;
    uint32_t core, rate_hz;
    uint64_t count, consumed, gaps, dropped, missed;
    uint32_t latency_max_ns, latency_p9999_ns, jitter_max_ns, jitter_p9999_ns;
};
#define SS_CONTROL_BINS 192u /* four counter ticks (167 ns) per bin, 32 us; the last bin takes the rest */
#define SS_CONTROL_BIN_TICKS 4u
enum ss_control_mode {
    /* Keep histograms without allocating a ring consumer or publishing ring samples. */
    SS_CONTROL_HISTOGRAM_ONLY = 1u
};
enum ss_control_error {
    SS_CONTROL_ERROR_DEADLINE = 1u,
    SS_CONTROL_ERROR_NOTIFICATION = 2u,
    SS_CONTROL_ERROR_NS_RANGE = 4u,
    SS_CONTROL_ERROR_TASK_CREATE = 8u
};
/* Counts and acknowledgments are release-published by their respective writers.
 * Quiescent bits require both the local timer-ISR and requested task-path writer
 * to acknowledge stop. consumer_parked follows the final ring drain. */
struct ss_control_progress {
    uint64_t frequency_hz, count[SS_CONTROL_CORES][2];
    uint32_t requested_mask, armed_mask, quiescent_mask, consumer_parked;
    uint32_t errors[SS_CONTROL_CORES];
};
struct ss_control_metric {
    uint64_t count;
    uint32_t maximum_ticks, p9999_upper_ticks;
};
/* Paths to run per core: bit 0 the control interrupt handler, bit 1 the woken task. */
unsigned ulSunstoneControlStartPaths(const unsigned *cores, const unsigned *rate_hz, const unsigned *paths, unsigned n);
/* The measurement mode is selected on the first start and cannot change.
 * A stopped instance cannot restart. A rate must produce a nonzero period. */
unsigned ulSunstoneControlStartPathsMode(const unsigned *cores, const unsigned *rate_hz,
                                       const unsigned *paths, unsigned n, unsigned mode);
unsigned ulSunstoneControlProgressRead(struct ss_control_progress *out);
unsigned ulSunstoneControlQuiescent(void);
/* Qualification-only completion check using the public FreeRTOS task-state
 * query. Returns zero when INCLUDE_eTaskGetState is disabled. Its queries can
 * add diagnostic contention; data quiescence is the separate promise above. */
unsigned ulSunstoneControlTasksSuspended(void);
/* Read after Quiescent returns true. Kinds match Histogram below. */
unsigned ulSunstoneControlMetricRead(unsigned core, unsigned path, unsigned kind,
                                    struct ss_control_metric *out);
/* The latency (kind 0) or jitter (kind 1) histogram of a path, SS_CONTROL_BINS counts with width SS_CONTROL_BIN_TICKS. The task
 * path also has kind 2, the handler's entry delay after the deadline, and kind 3, the task start after the handler.
 * Histogram and the legacy statistics/context getters require caller synchronization
 * with their writers; stop plus Quiescent provides that synchronization. */
const uint32_t *ulSunstoneControlHistogram(unsigned core, unsigned path, unsigned kind);
/* Start `rate_hz[i]` on `cores[i]` (not core 0). Returns the number started. Call once, from a task. */
unsigned ulSunstoneControlStart(const unsigned *cores, const unsigned *rate_hz, unsigned n);
/* Request stop and wake each owner/task. The caller polls Quiescent; this
 * function does not wait. Owned stale timer interrupts remain handled. */
void vSunstoneControlStop(void);
struct ss_control_worst { uint32_t latency_ns, pmr; uint64_t elr, spsr, rpr, task, nesting, when_us; };
/* The rank-th largest control-path latency of a core with the interrupted context (see runtime/control_irq.c). */
unsigned ulSunstoneControlWorst(unsigned core, unsigned rank, struct ss_control_worst *out);
/* The rank-th largest task-path handler entry delay of a core with the interrupted context. */
unsigned ulSunstoneTaskEntryWorst(unsigned core, unsigned rank, struct ss_control_worst *out);
/* Time spent in a phase of the wake path on a CPU (fork images built with SUNSTONE_OWNER_COUNTERS): field 0 count, 1 sum, 2 longest,
 * 3 count over 5 us, in counter ticks. Phases: 0 notification, 1 interrupt-exit work, 2 service pass, 3 control drain, 4 incoming
 * hints, 5 idle pull, 6 deadline expiry, 7 slice refresh, 8 yield tail, 9 task switch. */
uint64_t ulSunstonePhase(unsigned cpu, unsigned phase, unsigned field);
/* Refills of the woken task's wake, by gap class (see runtime/control_irq.c): field 0 wakes, 1 L1D refills, 2 L2D refills. */
uint64_t ulSunstoneTaskRefills(unsigned core, unsigned klass, unsigned field);
/* Owner-work drains of a CPU: which 0 from interrupt exit, 1 skipped at exit by the mask guard, 2 from the tick, 3 service passes run. */
uint64_t ulSunstoneOwnerCounters(unsigned cpu, unsigned which);
unsigned ulSunstoneControlWfi(unsigned core, uint64_t *count, uint64_t *max_ticks, uint64_t *over5, const uint32_t **bins);
unsigned long ulSunstoneIdleWaiting(unsigned core);
unsigned long ulSunstoneIdlePulls(void);
unsigned ulSunstoneControlStatsRead(unsigned core, unsigned path, struct ss_control_stats *out);
#endif
