/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SS_SCHEDULER_OWNER_H
#define SS_SCHEDULER_OWNER_H
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>

#define SS_OWNER_PRIORITIES 8u
#define SS_OWNER_SLOTS 128u
#define SS_OWNER_BATCH 16u

/* Callers mask local API interrupts around owner mutations and FIFO admission.
 * At most one non-preemptive participant per CPU may enter an object gate.
 * AArch64 builds require -march=armv8.1-a+lse and lock-free 64-bit atomics. */
struct ss_owner_backend {
    void (*barrier)(void *); /* Full store-to-load ordering; DMB ISH on Arm. */
    void (*wait)(void *);    /* WFE; admitted holders must make bounded progress. */
    void (*event)(void *);   /* Complete release writes, then SEV (DSB ISHST). */
    void (*sgi)(void *, unsigned); /* Completes DSB ISHST before SGI write. */
    void *context;
};
void ss_owner_barrier(const struct ss_owner_backend *);
void ss_owner_wait(const struct ss_owner_backend *);
void ss_owner_event(const struct ss_owner_backend *);

struct ss_owner_task {
    struct ss_owner_task *previous, *next;
    void *context;
    unsigned slot, priority;
    bool queued;
};
struct ss_owner_fifo { struct ss_owner_task *head, *tail; };
/* Cache-line groups. Another core writes the second group on every wake; the
 * first and third groups belong to the owner alone, so a remote publication
 * never invalidates the lines the owner selects from. */
#define SS_CACHE_LINE 64u
struct ss_owner {
    /* A local wake and a selection read the current task, the bitmap and one
     * FIFO. The FIFOs are stored highest priority first, so priorities 7 to 5
     * share the first line with current and the bitmap; use ss_owner_ready(). */
    _Alignas(SS_CACHE_LINE) struct ss_owner_task *current;
    uint32_t ready_bitmap;
    struct ss_owner_fifo ready_by_rank[SS_OWNER_PRIORITIES];
    _Alignas(SS_CACHE_LINE) _Atomic uint64_t incoming[SS_OWNER_PRIORITIES][2];
    /* Logical wake-list head: sixteen bucket bits. Slot bits precede their
     * head publication; owner detach may harmlessly consume a slot early. */
    _Atomic uint64_t incoming_words;
    _Atomic uint64_t dispatch;
    _Atomic uint64_t sgi_pending;
    _Alignas(SS_CACHE_LINE) uint64_t detached[SS_OWNER_PRIORITIES][2];
    unsigned cursor[SS_OWNER_PRIORITIES];
    uint64_t sequence;
    unsigned cpu;
    const struct ss_owner_backend *backend;
};
static inline struct ss_owner_fifo *ss_owner_ready(struct ss_owner *o, unsigned priority)
{ return &o->ready_by_rank[SS_OWNER_PRIORITIES - 1u - priority]; }
void ss_owner_init(struct ss_owner *, unsigned, const struct ss_owner_backend *);
bool ss_owner_enqueue(struct ss_owner *, struct ss_owner_task *, bool front);
bool ss_owner_remove(struct ss_owner *, struct ss_owner_task *);
/* Select excludes the returned running task from every ready list. */
struct ss_owner_task *ss_owner_select(struct ss_owner *);
struct ss_owner_task *ss_owner_yield(struct ss_owner *);
/* Requeue interrupted current at the head; yielding uses the tail. */
void ss_owner_preempt(struct ss_owner *);
void ss_owner_publish(struct ss_owner *, unsigned slot, unsigned priority);
/* Inspections validate current descriptor/wait state; hints never authorize a
 * wake. Each call inspects at most SS_OWNER_BATCH hints, highest priority first.
 * The callback alone decides whether to enqueue under local IRQ exclusion.
 * Incoming scan cost is 16 acquire loads initially, then for each subsequent
 * attempt: 16 loads if detached is empty, otherwise 2*(7-p) for its highest
 * detached priority p. At most 16 attempts give at most 256 loads; each nonzero
 * load adds one exchange. Sixteen priority-7 hints already detached require
 * only the initial 16 loads and their nonzero-word exchanges. */
typedef void (*ss_owner_inspect)(void *, unsigned slot, unsigned hint_priority);
unsigned ss_owner_drain(struct ss_owner *, ss_owner_inspect, void *);
bool ss_owner_has_pending(const struct ss_owner *);
/* Publish selected priority (0..7) or idle, clear SGI pending, DMB, inspect.
 * Any remaining publication requires a bounded continuation, including work
 * below the selected priority. True requires another owner entry before
 * committing task execution/WFI.
 * The final call and WFI must retain DAIF.I masking and admit the SGI at GIC. */
bool ss_owner_commit_dispatch(struct ss_owner *, unsigned priority, bool idle);
void ss_owner_continue(struct ss_owner *);

/* Permanent descriptor storage and incoming words must outlive all publishers.
 * A valid task handle is required before pin. No TCB field may be read before
 * pin succeeds. Close cannot free storage: wait for references, then drain and
 * acknowledge every owner's live/detached/control hints before reopen. */
/* Fixed routing to a retirement coordinator, independent of mutable task
 * placement. Configuration and context live for the permanent descriptor's
 * lifetime. Reopen and migration never change this record. The callback can
 * run from a rejected pin, task or API IRQ and must only publish permanent
 * control state and force a coordinator wake; it must not dereference a TCB.
 * Close and last-put may both notify, so requests must coalesce safely. */
struct ss_owner_retirement {
    void (*notify)(void *context, unsigned slot, unsigned retirement_owner);
    void *context;
    unsigned slot, retirement_owner;
};
/* One cache line per descriptor: a pin touches references, admission,
 * incarnation, task and the registry's lookup key together. */
struct ss_owner_descriptor {
    _Alignas(64) _Atomic uint64_t references;
    _Atomic uint64_t admission;
    _Atomic uint64_t incarnation;
    void *task;
    struct ss_owner_retirement retirement;
    _Atomic uintptr_t key; /* Handle registry's lookup key; the owner code never reads it. */
};
_Static_assert(sizeof(struct ss_owner_descriptor) == 64 || sizeof(void *) != 8,
               "a descriptor occupies exactly one 64-byte line");
/* Initialize each permanent descriptor once; the legacy open initializer has
 * no retirement callback. Registry initialization uses the closed initializer,
 * then opens admission only after its fixed control routing is available. */
void ss_owner_descriptor_init(struct ss_owner_descriptor *, void *, uint64_t);
void ss_owner_descriptor_init_closed(struct ss_owner_descriptor *, uint64_t,
                                     const struct ss_owner_retirement *);
bool ss_owner_pin(struct ss_owner_descriptor *, uint64_t,
                  const struct ss_owner_backend *);
/* Last release after closure notifies even when pin rejected admission. */
void ss_owner_put(struct ss_owner_descriptor *);
/* Close also notifies when references already equal zero. */
void ss_owner_close(struct ss_owner_descriptor *, const struct ss_owner_backend *);
bool ss_owner_references_retired(const struct ss_owner_descriptor *);
/* Administrative caller supplies proof of all owner acknowledgments. */
bool ss_owner_reopen(struct ss_owner_descriptor *, void *, uint64_t,
                     bool owners_drained);
#endif
