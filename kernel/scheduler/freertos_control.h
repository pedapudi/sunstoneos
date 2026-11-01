/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SS_FREERTOS_CONTROL_H
#define SS_FREERTOS_CONTROL_H
#include "owner.h"
#define SS_CONTROL_CPUS 64u
#define SS_CONTROL_NONE 64u
/* Slow-path callers exclude local API IRQs only during publish/lock service.
 * No object lock may be held. Owner callbacks never wait for another owner.
 * Initialize/register only under registry administration exclusion. Registration
 * requires all previous references and owner hints retired. All masks must be
 * nonempty subsets of active_mask; epochs and sequences must not wrap while
 * references exist. Each CPU has one owner consumer and one gate caller.
 * Progress requires bounded non-preemptive metadata service, owner IRQ/checkpoint
 * response, saved-frame eviction, and eventual publisher reference release.
 * Host checks exercise this mechanism; linked FreeRTOS adapters, IRQ return,
 * weak-memory modeling and physical timing require separate qualification. */
struct ss_control_state {
    uint64_t sequence, incarnation, affinity, resume_generation, abort_generation;
    uint64_t offcpu_sequence; /* outstanding suspend fence survives resume */
    unsigned priority, effective_priority;
    bool suspended, dead;
};
struct ss_control_transfer {
    uint64_t epoch, deadline, wait_generation, wait_outcome, accounting;
    unsigned old_owner, new_owner;
};
struct ss_control_ops {
    /* apply returns false until every offcpu_sequence fence is acknowledged,
     * even if a later resume cleared suspended. It reloads committed donation
     * records and repairs ready/deadline/active-object order atomically at owner entry.
     * detach returns false until saved frame is off CPU; it preserves wait result.
     * adopt rechecks authoritative object completion before making runnable. */
    bool (*apply)(void *, unsigned, void *, const struct ss_control_state *);
    bool (*detach)(void *, unsigned, void *, struct ss_control_transfer *);
    void (*adopt)(void *, unsigned, void *, const struct ss_control_transfer *);
    bool (*drained)(void *, unsigned, unsigned); /* local CPU only; live/detached hints */
    void (*retired)(void *, unsigned); /* no TCB dereference; retry registry retire */
    bool (*dispatch)(void *, unsigned); /* drain local pending; return local yield */
    void *context;
};
struct ss_control_slot {
    _Atomic uint32_t tickets, serving;
    struct ss_control_state desired;
    _Atomic uint64_t applied;
    struct ss_owner_descriptor *descriptor;
    struct ss_control_transfer transfer;
    _Atomic unsigned owner;
    unsigned phase; /* 0 stable, 1 detach, 2 adopt, 3 forwarding */
    bool retained, old_drained;
    uint64_t drain_mask;
};
enum ss_control_gate_phase { SS_GATE_OPEN, SS_GATE_ACQUIRING, SS_GATE_HELD, SS_GATE_RELEASING };
struct ss_control {
    struct ss_control_slot slot[SS_OWNER_SLOTS];
    _Atomic uint64_t pending[SS_CONTROL_CPUS][2];
    unsigned cursor[SS_CONTROL_CPUS];
    const struct ss_owner_backend *backend;
    struct ss_control_ops ops;
    uint64_t active_mask;
    _Atomic uint32_t gate_tickets, gate_serving;
    _Atomic unsigned gate_phase;
    _Atomic uint64_t gate_ack, gate_epoch;
    _Atomic uintptr_t gate_holder;
    _Atomic uint64_t gate_incarnation;
    unsigned gate_depth, boot_depth;
};
void ss_control_init(struct ss_control *, uint64_t, const struct ss_owner_backend *, const struct ss_control_ops *);
/* Called by registry initializer before publishing handle. */
void ss_control_register(struct ss_control *, unsigned, struct ss_owner_descriptor *, uint64_t, unsigned, unsigned, uint64_t);
/* Caller owns a successful registry pin, so descriptor/task are live. Mutation
 * executes under the FIFO metadata lock and may commit logical object state;
 * it must obey control->object lock order and must never wait for an owner.
 * false means no logical change (e.g. resume of nonsuspended task). */
typedef bool (*ss_control_mutate)(void *, struct ss_control_state *);
uint64_t ss_control_publish(struct ss_control *, unsigned, ss_control_mutate, void *);
void ss_control_snapshot(struct ss_control *, unsigned, struct ss_control_state *);
bool ss_control_applied(const struct ss_control *, unsigned, uint64_t);
void ss_control_wait(struct ss_control *, unsigned, uint64_t); /* task only, IRQs enabled */
unsigned ss_control_drain(struct ss_control *, unsigned); /* max sixteen records */
void ss_control_forward(struct ss_control *, unsigned); /* stale old-owner hint */
/* Bounded hint only; caller commits donation under object protocol first. */
void ss_control_repair(struct ss_control *, unsigned);
void ss_control_retirement(struct ss_control *, unsigned); /* last closed put hook */
bool ss_control_checkpoint(struct ss_control *, unsigned); /* true: defer dispatch */
/* One outstanding gate caller per CPU. Preemption disabled, IRQs enabled across
 * FIFO admission/wait. Identity is pinned task handle plus incarnation. */
void ss_control_suspend_all(struct ss_control *, unsigned, uintptr_t, uint64_t, bool);
bool ss_control_resume_all(struct ss_control *, unsigned, uintptr_t, uint64_t, bool);
#endif
