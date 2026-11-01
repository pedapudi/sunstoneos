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
struct ss_owner {
    struct ss_owner_fifo ready[SS_OWNER_PRIORITIES];
    struct ss_owner_task *current;
    uint32_t ready_bitmap;
    _Alignas(8) _Atomic uint64_t incoming[SS_OWNER_PRIORITIES][2];
    uint64_t detached[SS_OWNER_PRIORITIES][2];
    unsigned cursor[SS_OWNER_PRIORITIES];
    _Atomic uint64_t dispatch;
    _Atomic uint64_t sgi_pending;
    uint64_t sequence;
    unsigned cpu;
    const struct ss_owner_backend *backend;
};
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
 * True requires another owner entry before committing task execution/WFI.
 * The final call and WFI must retain DAIF.I masking and admit the SGI at GIC. */
bool ss_owner_commit_dispatch(struct ss_owner *, unsigned priority, bool idle);
void ss_owner_continue(struct ss_owner *);

/* Permanent descriptor storage and incoming words must outlive all publishers.
 * A valid task handle is required before pin. No TCB field may be read before
 * pin succeeds. Close cannot free storage: wait for references, then drain and
 * acknowledge every owner's live/detached/control hints before reopen. */
struct ss_owner_descriptor {
    _Atomic uint64_t references;
    _Atomic uint64_t admission;
    _Atomic uint64_t incarnation;
    void *task;
};
void ss_owner_descriptor_init(struct ss_owner_descriptor *, void *, uint64_t);
bool ss_owner_pin(struct ss_owner_descriptor *, uint64_t,
                  const struct ss_owner_backend *);
void ss_owner_put(struct ss_owner_descriptor *);
void ss_owner_close(struct ss_owner_descriptor *, const struct ss_owner_backend *);
bool ss_owner_references_retired(const struct ss_owner_descriptor *);
/* Administrative caller supplies proof of all owner acknowledgments. */
bool ss_owner_reopen(struct ss_owner_descriptor *, void *, uint64_t,
                     bool owners_drained);
#endif
