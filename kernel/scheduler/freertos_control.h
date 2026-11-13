/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SS_FREERTOS_CONTROL_H
#define SS_FREERTOS_CONTROL_H
#include "owner.h"
/* The native adapter admits at most the A733's eight CPUs. */
#define SS_CONTROL_CPUS 8u
#define SS_CONTROL_NONE 64u
#define SS_CONTROL_SUSPENDED 1u
#define SS_CONTROL_OFFCPU 2u
#define SS_CONTROL_SELF 4u
#define SS_CONTROL_SELF_FRAME 8u
#define SS_CONTROL_REMOTE_FRAME 16u
/* Slow-path callers exclude local API IRQs only during publish/lock service.
 * No object lock may be held. Owner callbacks never wait for another owner.
 * Initialize/register only under registry administration exclusion. Registration
 * requires all previous references and owner hints retired. All masks must be
 * nonempty subsets of active_mask; epochs and sequences must not wrap while
 * references exist. Each CPU has one owner consumer and one gate caller.
 * Progress requires bounded non-preemptive metadata service, owner IRQ/checkpoint
 * response, saved-frame eviction, and eventual publisher reference release.
 * A descriptor still referenced after delete acknowledgment must publish
 * ss_control_retirement on its final put; the kernel registry installs that
 * callback. Owners do not poll references with self-renewing interrupts.
 * Host checks exercise this mechanism; linked FreeRTOS adapters, IRQ return,
 * weak-memory modeling and physical timing require separate qualification. */
struct ss_control_state {
    uint64_t sequence, incarnation, affinity, resume_generation, abort_generation;
    uint64_t abort_wait_generation;
    uint64_t offcpu_sequence; /* outstanding suspend fence survives resume */
    unsigned priority, effective_priority;
    /* Idle pull: 1 + the CPU that should adopt this ready task, or 0. The owner clears it
     * when it starts the acknowledged transfer. */
    unsigned move_to_plus_one;
    bool suspended, dead, preemption_disabled;
};
struct ss_control_transfer {
    uint64_t epoch, deadline, wait_generation, wait_outcome, accounting, execution;
    uint64_t route_epoch; /* closed publisher epoch, independent of incarnation */
    unsigned old_owner, new_owner;
};
struct ss_control_ops {
    /* Optional preparation runs with the API mask open and the slot FIFO
     * retained. False parks this record until its resource release. */
    bool (*prepare)(void *, unsigned cpu, unsigned slot);
    /* apply returns false until every offcpu_sequence fence is acknowledged,
     * even if a later resume cleared suspended. It reloads committed donation
     * records and repairs ready/deadline/active-object order atomically at owner entry.
     * detach returns false until saved frame is off CPU; it preserves wait result.
     * adopt rechecks authoritative object completion before making runnable. */
    bool (*apply)(void *, unsigned, void *, const struct ss_control_state *);
    /* A false apply can wait on a task-context resource destructor. Its
     * release republishes work; do not issue self-renewing SGIs meanwhile. */
    bool (*deferred)(void *, unsigned);
    bool (*detach)(void *, unsigned, void *, struct ss_control_transfer *);
    void (*adopt)(void *, unsigned, void *, const struct ss_control_transfer *);
    bool (*drained)(void *, unsigned, unsigned); /* local CPU only; live/detached hints */
    /* No TCB dereference; registry administration rejects a callback from an
     * earlier incarnation before retrying retirement of this slot. */
    void (*retired)(void *, unsigned, uint64_t incarnation);
    bool (*dispatch)(void *, unsigned); /* drain local pending; return local yield */
    bool (*pause)(void *, unsigned); /* stop this batch before another record */
    void *context;
};
struct ss_control_slot {
    /* A wake reads only this first line: the route word, a reader bank and the
     * affinity. Slots are line-aligned so the count holds for every slot. */
    /* Low eight route-word bits are the owner; upper bits are the epoch.
     * Reader banks combine CLOSED in bit 63 with their participant count. */
    _Alignas(64) _Atomic uint64_t route_word;
    _Atomic uint64_t route_readers[2];
    _Atomic uint64_t affinity;
    _Atomic uint32_t tickets, serving;
    _Atomic uint64_t waiters;
    /* One authoritative state/source word. Resume clears SUSPENDED and SELF,
     * preserving OFFCPU and the frame-source bits through saved-frame commit.
     * Self publication and ISR resume never wait for this metadata FIFO. */
    _Atomic unsigned suspension;
    _Atomic uint64_t resume_pending;
    struct ss_control_state desired;
    _Atomic uint64_t applied;
    struct ss_owner_descriptor *descriptor;
    struct ss_control_transfer transfer;
    _Atomic unsigned owner;
    unsigned phase; /* 0 stable, 1 detach, 2 adopt, 3 forwarding */
    bool retained, old_drained;
    uint64_t drain_mask;
};
struct ss_control_route {
    struct ss_control *control;
    uint64_t epoch;
    unsigned slot, owner;
};
enum ss_control_gate_phase { SS_GATE_OPEN, SS_GATE_ACQUIRING, SS_GATE_HELD, SS_GATE_RELEASING };
struct ss_control {
    struct ss_control_slot slot[SS_OWNER_SLOTS];
    /* Remote publishers set the pending words; the owner alone moves its
     * cursor. One cache line each, so no owner shares a line with another. */
    struct { _Alignas(64) _Atomic uint64_t word[2]; } pending[SS_CONTROL_CPUS];
    struct { _Alignas(64) _Atomic uint64_t word[2]; } parked[SS_CONTROL_CPUS];
    _Atomic uintptr_t parked_on[SS_CONTROL_CPUS][SS_OWNER_SLOTS];
    struct { _Alignas(64) unsigned value; } cursor[SS_CONTROL_CPUS];
    struct ss_control_ops ops;
    uint64_t active_mask;
    /* A wake reads the backend and the gate phase; keep them on one line. */
    _Alignas(64) const struct ss_owner_backend *backend;
    _Atomic unsigned gate_phase;
    _Atomic uint32_t gate_tickets, gate_serving;
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
/* Current execution already protects the self task. No counted reference may
 * be left on its stack. The caller performs its local transition and yields. */
bool ss_control_suspend_current(struct ss_control *, unsigned slot, unsigned cpu);
/* A pinned ISR caller publishes logical resume without taking the FIFO. */
bool ss_control_resume_isr(struct ss_control *, unsigned slot);
unsigned ss_control_suspension(const struct ss_control *, unsigned slot);
/* FIFO holder only. Refresh again immediately before the masked owner commit. */
void ss_control_refresh_suspension(struct ss_control *, unsigned slot);
/* A failed preparation waits for its resource release, not another SGI retry. */
void ss_control_park(struct ss_control *, unsigned cpu, unsigned slot);
void ss_control_wake(struct ss_control *, unsigned cpu, unsigned slot);
void ss_control_park_on(struct ss_control *, unsigned cpu, unsigned slot, const void *resource);
void ss_control_wake_on(struct ss_control *, unsigned cpu, unsigned slot, const void *resource);
void ss_control_wake_mask(struct ss_control *, unsigned cpu, const uint64_t slots[2], const void *resource);
void ss_control_snapshot(struct ss_control *, unsigned, struct ss_control_state *);
bool ss_control_applied(const struct ss_control *, unsigned, uint64_t);
void ss_control_wait(struct ss_control *, unsigned, uint64_t); /* task only, IRQs enabled */
unsigned ss_control_drain(struct ss_control *, unsigned); /* max sixteen records */
/* Shared visited words define one maintenance epoch. Reissued hints remain
 * pending but cannot cause a second visit before a new epoch. */
unsigned ss_control_drain_excluding(struct ss_control *, unsigned, uint64_t visited[2]);
/* Consume only this owner's hint for one registered slot. A hint reissued by
 * consume remains pending; the ordinary scan cursor and other slots are unchanged. */
unsigned ss_control_drain_slot(struct ss_control *, unsigned cpu, unsigned slot);
bool ss_control_has_pending(const struct ss_control *, unsigned cpu);
void ss_control_forward(struct ss_control *, unsigned); /* stale old-owner hint */
/* Caller already owns a lifetime pin and masks local API interrupts. One
 * registration/validation attempt; false requires an epoch-neutral bounded
 * broadcast activation hint. Keep the lifetime pin until route_put and every
 * slot publication/SGI finish. Never retry a failed routing admission. */
bool ss_control_route_pin(struct ss_control *, unsigned, struct ss_control_route *);
void ss_control_route_put(struct ss_control_route *);
/* Bounded hint only; caller commits donation under object protocol first. */
void ss_control_repair(struct ss_control *, unsigned);
void ss_control_retirement(struct ss_control *, unsigned); /* last closed put hook */
bool ss_control_checkpoint(struct ss_control *, unsigned); /* true: defer dispatch */
/* One outstanding gate caller per CPU. Preemption disabled, IRQs enabled across
 * FIFO admission/wait. Identity is pinned task handle plus incarnation. */
void ss_control_suspend_all(struct ss_control *, unsigned, uintptr_t, uint64_t, bool);
bool ss_control_resume_all(struct ss_control *, unsigned, uintptr_t, uint64_t, bool);
#endif
