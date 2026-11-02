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
#define SS_CONTROL_BINS 768u /* one counter tick (41.7 ns) per bin, the last bin takes the rest */
/* Paths to run per core: bit 0 the control interrupt handler, bit 1 the woken task. */
unsigned ulSunstoneControlStartPaths(const unsigned *cores, const unsigned *rate_hz, const unsigned *paths, unsigned n);
/* The latency (kind 0) or jitter (kind 1) histogram of a path, SS_CONTROL_BINS counts of one tick each. */
const uint32_t *ulSunstoneControlHistogram(unsigned core, unsigned path, unsigned kind);
/* Start `rate_hz[i]` on `cores[i]` (not core 0). Returns the number started. Call once, from a task. */
unsigned ulSunstoneControlStart(const unsigned *cores, const unsigned *rate_hz, unsigned n);
void vSunstoneControlStop(void);
unsigned ulSunstoneControlStatsRead(unsigned core, unsigned path, struct ss_control_stats *out);
#endif
