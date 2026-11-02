/* SPDX-License-Identifier: Apache-2.0 */
/* Hard real-time control interrupts above the kernel API mask (SUNSTONE_CONTROL_IRQ, measurement).
 * A core-local timer interrupt runs a handler that calls no kernel API, measures its own latency and
 * period jitter, and hands samples to a task through a lock-free ring. */
#ifndef SUNSTONE_CONTROL_H
#define SUNSTONE_CONTROL_H
#include <stdint.h>
#define SS_CONTROL_CORES 8u
struct ss_control_stats {
    uint32_t core, rate_hz;
    uint64_t count, consumed, gaps, dropped, missed;
    uint32_t latency_max_ns, latency_p9999_ns, jitter_max_ns, jitter_p9999_ns;
};
/* Start `rate_hz[i]` on `cores[i]` (not core 0). Returns the number started. Call once, from a task. */
unsigned ss_control_start(const unsigned *cores, const unsigned *rate_hz, unsigned n);
void ss_control_stop(void);
unsigned ss_control_stats_read(unsigned core, struct ss_control_stats *out);
#endif
