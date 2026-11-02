/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SS_SCHEDULER_DEADLINE_H
#define SS_SCHEDULER_DEADLINE_H
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>

#define SS_DEADLINE_CAPACITY 128u
#define SS_DEADLINE_NONE UINT16_MAX
#define SS_DEADLINE_BATCH 16u
struct ss_deadline_heap;
struct ss_deadline_entry {
    void *context;
    uint64_t generation, deadline, order;
    struct ss_deadline_heap *owner;
    uint16_t index;
};
struct ss_deadline_result {
    void *context;
    uint64_t generation, deadline;
};
enum ss_deadline_state {
    SS_DEADLINE_UNKNOWN, SS_DEADLINE_EMPTY, SS_DEADLINE_VALUE
};
struct ss_deadline_publication {
    _Atomic uint64_t sequence, deadline, present;
};
struct ss_deadline_heap {
    struct ss_deadline_entry *entries[SS_DEADLINE_CAPACITY];
    uint64_t order;
    uint16_t count;
    struct ss_deadline_publication published;
};
/* All mutations and local queries require this heap's owner CPU with local API
 * interrupts excluded. Entry storage outlives membership. Each entry represents
 * one task wait. Never initialize a member entry or an occupied heap again.
 * Absolute deadlines and now use one monotonic unsigned 64-bit boot era. The
 * adapter extends wrapping FreeRTOS TickType_t ticks into that era, in 1 ms
 * units for stage B. Every valid finite TickType_t delay fits; indefinite waits
 * are handled separately by the API adapter. Stage D uses counter units and
 * converts relative ticks without truncation. No operation wraps this era;
 * adapters reject arithmetic overflow explicitly rather than clamping delays.
 * A wrapping hardware counter needs era extension before calling this module.
 * The adapter checks a newly inserted deadline against now before sleeping or
 * programming its owner compare. This module never accesses clock or MMIO. */
void ss_deadline_init(struct ss_deadline_heap *);
void ss_deadline_entry_init(struct ss_deadline_entry *);
bool ss_deadline_insert(struct ss_deadline_heap *, struct ss_deadline_entry *,
                        void *context, uint64_t generation, uint64_t deadline);
/* A stale cancellation cannot remove a reused entry's newer generation. */
bool ss_deadline_cancel(struct ss_deadline_heap *, struct ss_deadline_entry *,
                        uint64_t generation);
bool ss_deadline_pop_expired(struct ss_deadline_heap *, uint64_t now,
                             struct ss_deadline_result *);
/* Removes each entry before invoking its callback. The callback arbitrates the
 * full generation against the task/object wait state before any wake. It may
 * cancel or reinsert entries under the same owner exclusion. Output snapshots
 * never refer to mutable entry fields. At most min(budget,16) callbacks run;
 * an overdue root afterward requires an owner continuation before sleeping. */
typedef void (*ss_deadline_expired)(void *, const struct ss_deadline_result *);
unsigned ss_deadline_expire(struct ss_deadline_heap *, uint64_t now,
                            unsigned budget, ss_deadline_expired, void *);
bool ss_deadline_next(const struct ss_deadline_heap *, uint64_t *);
/* Remote readers make one sequence snapshot, with no retry. UNKNOWN requires
 * conservative owner delivery by the stage-B tick adapter. Init precedes remote
 * access; fewer than 2^63 publications may overlap a reader (sequence ABA).
 * Native aligned uint64_t atomic loads/stores must be lock-free. */
enum ss_deadline_state ss_deadline_snapshot(
    const struct ss_deadline_publication *, uint64_t *);
/* Relative addition preserves delays beyond the signed half-range. */
bool ss_deadline_after(uint64_t now, uint64_t delta, uint64_t *absolute);
#endif
