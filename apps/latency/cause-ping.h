#ifndef SUNSTONE_LATENCY_CAUSE_PING_H
#define SUNSTONE_LATENCY_CAUSE_PING_H

#include <stdbool.h>
#include <stdint.h>

#define LATENCY_CAUSE_PING_MAGIC UINT64_C(0x535350494e473031)
#define LATENCY_CAUSE_PING_SAMPLES 256u
#define LATENCY_CAUSE_PING_PAIRS 2u
#define LATENCY_CAUSE_PING_BYTES 3392u
/* One quarter of the latency manifest's one-second progress window. */
#define LATENCY_CAUSE_PING_TIMEOUT_US 250000u

enum latency_cause_ping_status {
    LATENCY_CAUSE_PING_PENDING,
    LATENCY_CAUSE_PING_OK,
    LATENCY_CAUSE_PING_WRONG_CORE,
    LATENCY_CAUSE_PING_NOT_READY,
    LATENCY_CAUSE_PING_TIMEOUT,
    LATENCY_CAUSE_PING_SEQUENCE,
    LATENCY_CAUSE_PING_CYCLE_RANGE,
    LATENCY_CAUSE_PING_ABORTED
};
/* Clock/type groups from the admitted A733 topology, not separate DSUs. */
enum latency_cause_ping_group { LATENCY_CAUSE_PING_CPU_L, LATENCY_CAUSE_PING_CPU_B };

struct latency_cause_ping_reference {
    uint64_t core, group, status, completed, begin_tick, end_tick, irq_begin, irq_end;
    uint32_t cycles[LATENCY_CAUSE_PING_SAMPLES];
};
struct latency_cause_ping_pair {
    uint64_t source_core, target_core, source_group, target_group, status, completed;
    uint64_t begin_tick, end_tick, source_irq_begin, source_irq_end, target_irq_begin, target_irq_end;
    uint32_t cycles[LATENCY_CAUSE_PING_SAMPLES];
};
struct latency_cause_ping_report {
    uint64_t magic, version, bytes, samples, frequency, begin_tick, end_tick, status;
    struct latency_cause_ping_reference reference;
    struct latency_cause_ping_pair pair[LATENCY_CAUSE_PING_PAIRS];
};
_Static_assert(sizeof(struct latency_cause_ping_report) == LATENCY_CAUSE_PING_BYTES,
               "cause ping report layout");

/* Initialize before scheduling. Start once, after memory workers stop and before
 * recorder freeze. The controller polls done with its ordinary one-tick delay.
 * One overall 250ms raw-counter deadline bounds readiness and exchanges. A
 * one-tick (1ms) parking allowance leaves 749ms of the one-second progress
 * window for the controller. Stop requests cancellation; IRQ and scheduler
 * delays remain observable rather than being subtracted from the budget.
 * Done also requires all three saved task frames to be suspended. Snapshot then
 * copies immutable results. Normal interrupts remain enabled throughout.
 *
 * Only CPU0 reads its own free-running PMCCNTR after cause_core_ready(0). Each
 * RTT includes two ISB/MRS timestamps. RTT/2 is an estimated per-transfer cost;
 * its difference from the local store/load reference still includes peer
 * instructions, polling and interrupt interference. No cross-CPU cycle values
 * are subtracted. IRQ totals bracket whole batches, not individual samples. */
void latency_cause_ping_init(void);
bool latency_cause_ping_start(uint64_t counter_frequency);
bool latency_cause_ping_done(void);
void latency_cause_ping_stop(void);
bool latency_cause_ping_snapshot(struct latency_cause_ping_report *out);

#endif
