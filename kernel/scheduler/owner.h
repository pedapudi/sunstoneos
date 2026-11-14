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
    /* The publication SGI for a hint of `priority` (kernel/scheduler/bands.h):
     * the SGI of the task's band, which a target running a higher band holds.
     * Completes DSB ISHST before the SGI write. NULL sends sgi. */
    void (*publish)(void *, unsigned cpu, unsigned priority);
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
#define SS_CACHE_LINE 64u
#define SS_OWNER_SOURCES 8u /* publishing CPUs */
#define SS_OWNER_RING 32u   /* hints per (source, owner) outbox */
/* What an owner reads on every entry, on one line that only other parties
 * write. published[s] counts the hints source CPU s has written to its outbox
 * (16-bit, modulo). limit[s] is source s's copy of the owner's consumed[s],
 * refreshed only when its outbox looks full. spilled counts hints that found
 * a full outbox and went to the spill bitmap instead. control and gate belong
 * to the control path (freertos_control.h): a count of requests made pending
 * and not parked on this CPU, and a copy of the gate phase. The owner never
 * writes this line, so under streaming load the copy that serves its reads
 * stays in the cache of the CPU that last published. */
struct ss_owner_inbox {
    _Alignas(SS_CACHE_LINE) _Atomic uint16_t published[SS_OWNER_SOURCES];
    _Atomic uint16_t limit[SS_OWNER_SOURCES];
    _Atomic uint32_t spilled, control, gate;
    /* Constant after init. Here so that a publisher's SGI reads no other
     * owner line. */
    unsigned cpu;
    const struct ss_owner_backend *backend;
};
/* Written only by its source: entry[n % SS_OWNER_RING] holds hint n as
 * slot | priority << 7. */
struct ss_owner_outbox {
    _Alignas(SS_CACHE_LINE) _Atomic uint16_t entry[SS_OWNER_RING];
};
_Static_assert(sizeof(struct ss_owner_inbox) == SS_CACHE_LINE &&
               sizeof(struct ss_owner_outbox) == SS_CACHE_LINE, "one line each");
/* Cache-line groups. The first group and the detached group belong to the
 * owner alone. Publishers write the inbox, their own outbox and, only when
 * that is full, the spill bitmap; the owner writes consumed, which a source
 * reads only when its outbox looks full. So a publication reads no line the
 * owner wrote, and the owner reads no line it wrote that another CPU must
 * fetch back. */
struct ss_owner {
    /* A local wake and a selection read the current task, the bitmap and one
     * FIFO. The FIFOs are stored highest priority first, so priorities 7 to 5
     * share the first line with current and the bitmap; use ss_owner_ready(). */
    _Alignas(SS_CACHE_LINE) struct ss_owner_task *current;
    uint32_t ready_bitmap;
    struct ss_owner_fifo ready_by_rank[SS_OWNER_PRIORITIES];
    struct ss_owner_inbox inbox;
    struct ss_owner_outbox outbox[SS_OWNER_SOURCES];
    _Alignas(SS_CACHE_LINE) _Atomic uint64_t spill[SS_OWNER_PRIORITIES][2];
    _Alignas(SS_CACHE_LINE) _Atomic uint16_t consumed[SS_OWNER_SOURCES];
    _Alignas(SS_CACHE_LINE) uint64_t detached[SS_OWNER_PRIORITIES][2];
    unsigned cursor[SS_OWNER_PRIORITIES];
    uint16_t taken[SS_OWNER_SOURCES]; /* owner's copy of consumed */
    uint32_t spilled_seen;
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
/* Publish a hint from source CPU `source` and signal the owner with an SGI,
 * every time: the owner's dispatch state is not read, because reading it
 * would fetch a line the owner wrote. The caller masks local API interrupts,
 * so each source has one writer. */
void ss_owner_publish(struct ss_owner *, unsigned source, unsigned slot, unsigned priority);
/* Inspections validate current descriptor/wait state; hints never authorize a
 * wake. Each call inspects at most SS_OWNER_BATCH hints, highest priority first.
 * The callback alone decides whether to enqueue under local IRQ exclusion.
 * Each merge reads the inbox line, then the outbox of each source whose count
 * moved, and the spill bitmap only when the spill count moved. A drain merges
 * before each inspection, so a hint published during the batch can outrank
 * the rest of it. */
typedef void (*ss_owner_inspect)(void *, unsigned slot, unsigned hint_priority);
unsigned ss_owner_drain(struct ss_owner *, ss_owner_inspect, void *);
/* Owner CPU only. True while a hint is unmerged or detached. */
bool ss_owner_has_pending(const struct ss_owner *);
/* Owner CPU only. The highest priority among unmerged, spilled and detached
 * hints, or -1 when none is pending. Unmerged hints are read from their
 * outbox entries, which costs a line per source whose count moved. */
int ss_owner_pending_top(const struct ss_owner *);
/* Owner CPU only. True while a hint for this slot is unmerged or detached. */
bool ss_owner_slot_pending(const struct ss_owner *, unsigned slot);
/* Signal the owner again when hints remain after its pass. */
void ss_owner_continue(struct ss_owner *);

/* Permanent descriptor storage and the owner mailbox must outlive all publishers.
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
