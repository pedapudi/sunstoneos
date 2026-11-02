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
/* Paths to run per core: bit 0 the control interrupt handler, bit 1 the woken task. */
unsigned ulSunstoneControlStartPaths(const unsigned *cores, const unsigned *rate_hz, const unsigned *paths, unsigned n);
/* The latency (kind 0) or jitter (kind 1) histogram of a path, SS_CONTROL_BINS counts of one tick each. */
const uint32_t *ulSunstoneControlHistogram(unsigned core, unsigned path, unsigned kind);
/* Start `rate_hz[i]` on `cores[i]` (not core 0). Returns the number started. Call once, from a task. */
unsigned ulSunstoneControlStart(const unsigned *cores, const unsigned *rate_hz, unsigned n);
void vSunstoneControlStop(void);
struct ss_control_worst { uint32_t latency_ns, pmr; uint64_t elr, spsr, rpr, task, nesting, when_us; };
/* The rank-th largest control-path latency of a core with the interrupted context (see runtime/control_irq.c). */
unsigned ulSunstoneControlWorst(unsigned core, unsigned rank, struct ss_control_worst *out);
/* Owner-work drains of a CPU: which 0 from interrupt exit, 1 skipped at exit by the mask guard, 2 from the tick, 3 service passes run. */
uint64_t ulSunstoneOwnerCounters(unsigned cpu, unsigned which);
unsigned ulSunstoneControlWfi(unsigned core, uint64_t *count, uint64_t *max_ticks, uint64_t *over5, const uint32_t **bins);
unsigned long ulSunstoneIdleWaiting(unsigned core);
unsigned long ulSunstoneIdlePulls(void);
unsigned ulSunstoneControlStatsRead(unsigned core, unsigned path, struct ss_control_stats *out);
#endif
