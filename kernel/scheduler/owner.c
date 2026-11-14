/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "owner.h"
#include "latency_cause.h"
#include "perturb.h"
#include <stddef.h>

_Static_assert(ATOMIC_LLONG_LOCK_FREE == 2, "native 64-bit atomics required");
#if defined(__aarch64__) && !defined(__ARM_FEATURE_ATOMICS)
#error "Owner publication requires native LSE atomics"
#endif
void ss_owner_barrier(const struct ss_owner_backend *b)
{
    if (b && b->barrier) { b->barrier(b->context); return; }
#if defined(__aarch64__)
    __asm__ volatile("dmb ish" ::: "memory");
#else
    atomic_thread_fence(memory_order_seq_cst);
#endif
}
void ss_owner_wait(const struct ss_owner_backend *b)
{
#ifdef SS_LATENCY_OPS
    extern uint64_t ss_ops_wait[8];
    ss_ops_wait[ss_ops_core()]++; /* host model: a masked span that waits is not an uncontended path */
#endif
    if (b && b->wait) { b->wait(b->context); return; }
#if defined(__aarch64__)
    __asm__ volatile("wfe" ::: "memory");
#else
    atomic_signal_fence(memory_order_seq_cst);
#endif
}
void ss_owner_event(const struct ss_owner_backend *b)
{
    if (b && b->event) { b->event(b->context); return; }
#if defined(__aarch64__)
    __asm__ volatile("dsb ishst\nsev" ::: "memory");
#endif
}
void ss_owner_init(struct ss_owner *o, unsigned cpu,
                   const struct ss_owner_backend *backend)
{
    *o = (struct ss_owner){0};
    o->inbox.cpu = cpu; o->inbox.backend = backend;
}
bool ss_owner_enqueue(struct ss_owner *o, struct ss_owner_task *t, bool front)
{
    if (!t || t->queued || t == o->current || t->priority >= 8 || t->slot >= 128)
        return false;
    struct ss_owner_fifo *q = ss_owner_ready(o, t->priority);
    t->previous = front ? NULL : q->tail;
    t->next = front ? q->head : NULL;
    if (front) {
        if (q->head) q->head->previous = t; else q->tail = t;
        q->head = t;
    } else {
        if (q->tail) q->tail->next = t; else q->head = t;
        q->tail = t;
    }
    t->queued = true; o->ready_bitmap |= 1u << t->priority;
    return true;
}
bool ss_owner_remove(struct ss_owner *o, struct ss_owner_task *t)
{
    if (!t || !t->queued || t->priority >= 8) return false;
    struct ss_owner_fifo *q = ss_owner_ready(o, t->priority);
    if (t->previous) t->previous->next = t->next; else q->head = t->next;
    if (t->next) t->next->previous = t->previous; else q->tail = t->previous;
    t->previous = t->next = NULL; t->queued = false;
    if (!q->head) o->ready_bitmap &= ~(1u << t->priority);
    return true;
}
struct ss_owner_task *ss_owner_select(struct ss_owner *o)
{
    if (o->current) return o->current;
    if (!o->ready_bitmap) return NULL;
    unsigned p = 31u - (unsigned)__builtin_clz(o->ready_bitmap);
    struct ss_owner_task *t = ss_owner_ready(o, p)->head;
    ss_owner_remove(o, t); o->current = t; return t;
}
static void requeue(struct ss_owner *o, bool front)
{
    struct ss_owner_task *t = o->current;
    o->current = NULL;
    if (t) (void)ss_owner_enqueue(o, t, front);
}
void ss_owner_preempt(struct ss_owner *o) { requeue(o, true); }
struct ss_owner_task *ss_owner_yield(struct ss_owner *o)
{ requeue(o, false); return ss_owner_select(o); }
static void signal(struct ss_owner *o)
{
#ifdef SUNSTONE_LATENCY_CAUSE
    ss_latency_cause_sgi_begin(o->inbox.cpu);
#endif
#if defined(__aarch64__)
    __asm__ volatile("dsb ishst" ::: "memory");
#endif
    const struct ss_owner_backend *b = o->inbox.backend;
    if (b && b->sgi) b->sgi(b->context, o->inbox.cpu);
#ifdef SUNSTONE_LATENCY_CAUSE
    ss_latency_cause_sgi_end(o->inbox.cpu);
#endif
}
void ss_owner_publish(struct ss_owner *o, unsigned source, unsigned slot, unsigned priority)
{
    if (source >= SS_OWNER_SOURCES || slot >= SS_OWNER_SLOTS || priority >= SS_OWNER_PRIORITIES)
        return;
    struct ss_owner_inbox *in = &o->inbox;
    /* This source is the only writer of published[source] and limit[source]. */
    uint16_t n = atomic_load_explicit(&in->published[source], memory_order_relaxed);
    uint16_t limit = atomic_load_explicit(&in->limit[source], memory_order_relaxed);
    if ((uint16_t)(n - limit) >= SS_OWNER_RING) {
        /* Pairs with the owner's release of consumed[source], which follows
         * its reads of the entries below it, so entry n % SS_OWNER_RING is
         * free once n - consumed[source] < SS_OWNER_RING. */
        limit = atomic_load_explicit(&o->consumed[source], memory_order_acquire);
        atomic_store_explicit(&in->limit[source], limit, memory_order_relaxed);
    }
    if ((uint16_t)(n - limit) < SS_OWNER_RING) {
        atomic_store_explicit(&o->outbox[source].entry[n % SS_OWNER_RING],
                              (uint16_t)(slot | priority << 7), memory_order_relaxed);
        atomic_store_explicit(&in->published[source], (uint16_t)(n + 1), memory_order_release);
    } else {
        /* The owner has not taken the last SS_OWNER_RING hints. The bit
         * precedes the count, so an owner that sees the count sees the bit. */
        atomic_fetch_or_explicit(&o->spill[priority][slot / 64],
                                 UINT64_C(1) << (slot % 64), memory_order_release);
        atomic_fetch_add_explicit(&in->spilled, 1, memory_order_release);
    }
    SS_KERNEL_PERTURB(SS_PERTURB_OWNER_PUBLISH);
    /* The backend completes DSB ISHST before the SGI write, so the owner's
     * exception entry, which takes the SGI, finds the hint. An SGI that is
     * already pending at the GIC is taken after this hint is visible too. */
    signal(o);
}
#ifdef SS_OWNER_TEST_STATS
/* Host fixtures count inbox loads, outbox entry loads and spill exchanges;
 * production emits no hook. */
extern void ss_owner_test_scan(unsigned kind);
#define SCAN_STAT(kind) ss_owner_test_scan(kind)
#else
#define SCAN_STAT(kind) ((void)0)
#endif
static void detach(struct ss_owner *o, uint16_t entry)
{
    unsigned slot = entry & 127u, priority = (entry >> 7) & 7u;
    o->detached[priority][slot / 64] |= UINT64_C(1) << (slot % 64);
}
static void merge(struct ss_owner *o)
{
    struct ss_owner_inbox *in = &o->inbox;
    for (unsigned s = 0; s < SS_OWNER_SOURCES; ++s) {
        SCAN_STAT(0);
        uint16_t n = atomic_load_explicit(&in->published[s], memory_order_acquire);
        uint16_t t = o->taken[s];
        if (n == t) continue;
        for (; t != n; ++t) {
            SCAN_STAT(1);
            detach(o, atomic_load_explicit(&o->outbox[s].entry[t % SS_OWNER_RING],
                                           memory_order_relaxed));
        }
        o->taken[s] = n;
        atomic_store_explicit(&o->consumed[s], n, memory_order_release);
    }
    SCAN_STAT(0);
    uint32_t spilled = atomic_load_explicit(&in->spilled, memory_order_acquire);
    if (spilled == o->spilled_seen) return;
    o->spilled_seen = spilled;
    for (unsigned p = 0; p < SS_OWNER_PRIORITIES; ++p)
        for (unsigned w = 0; w < 2; ++w)
            if (atomic_load_explicit(&o->spill[p][w], memory_order_relaxed)) {
                SCAN_STAT(2);
                o->detached[p][w] |= atomic_exchange_explicit(&o->spill[p][w], 0,
                                                              memory_order_acquire);
            }
}
static uint32_t detached_priorities(const struct ss_owner *o)
{
    uint32_t priorities = 0;
    for (unsigned p = 0; p < 8; ++p)
        if (o->detached[p][0] | o->detached[p][1]) priorities |= 1u << p;
    return priorities;
}
static unsigned first_bit(uint64_t bits)
{ return 63u - (unsigned)__builtin_clzll(bits & (0 - bits)); }
static unsigned next_slot(struct ss_owner *o, unsigned p)
{
    unsigned c = o->cursor[p], w = c / 64, b = c % 64;
    uint64_t bits = o->detached[p][w] & (UINT64_MAX << b);
    if (!bits) {
        w ^= 1; bits = o->detached[p][w];
        if (!bits) { w ^= 1; bits = o->detached[p][w]; }
    }
    unsigned slot = w * 64 + first_bit(bits);
    o->detached[p][w] &= ~(UINT64_C(1) << (slot % 64));
    o->cursor[p] = (slot + 1) % 128;
    return slot;
}
unsigned ss_owner_drain(struct ss_owner *o, ss_owner_inspect inspect, void *ctx)
{
    unsigned count = 0;
    merge(o);
    SS_KERNEL_PERTURB(SS_PERTURB_OWNER_DRAIN);
    for (; count < SS_OWNER_BATCH; ++count) {
        /* The cursor keeps a priority's sweep fair: a slot published again
         * behind it waits for the sweep to wrap. */
        if (count) merge(o);
        uint32_t priorities = detached_priorities(o);
        if (!priorities) break;
        unsigned p = 31u - (unsigned)__builtin_clz(priorities);
        inspect(ctx, next_slot(o, p), p);
    }
    return count;
}
static bool unmerged(const struct ss_owner *o)
{
    for (unsigned s = 0; s < SS_OWNER_SOURCES; ++s)
        if (atomic_load_explicit(&o->inbox.published[s], memory_order_acquire) != o->taken[s])
            return true;
    return atomic_load_explicit(&o->inbox.spilled, memory_order_acquire) != o->spilled_seen;
}
bool ss_owner_has_pending(const struct ss_owner *o)
{
    if (unmerged(o)) return true;
    for (unsigned p = 0; p < 8; ++p)
        if (o->detached[p][0] | o->detached[p][1]) return true;
    return false;
}
bool ss_owner_slot_pending(const struct ss_owner *o, unsigned slot)
{
    if (slot >= SS_OWNER_SLOTS) return false;
    uint64_t bit = UINT64_C(1) << (slot % 64);
    for (unsigned p = 0; p < SS_OWNER_PRIORITIES; ++p)
        if ((o->detached[p][slot / 64] & bit) ||
            (atomic_load_explicit(&o->spill[p][slot / 64], memory_order_acquire) & bit))
            return true;
    for (unsigned s = 0; s < SS_OWNER_SOURCES; ++s) {
        uint16_t n = atomic_load_explicit(&o->inbox.published[s], memory_order_acquire);
        for (uint16_t t = o->taken[s]; t != n; ++t)
            if ((atomic_load_explicit(&o->outbox[s].entry[t % SS_OWNER_RING],
                                      memory_order_relaxed) & 127u) == slot) return true;
    }
    return false;
}
void ss_owner_continue(struct ss_owner *o)
{ if (ss_owner_has_pending(o)) signal(o); }
static void descriptor_init(struct ss_owner_descriptor *d, void *task,
                            uint64_t i, bool admitted,
                            const struct ss_owner_retirement *retirement)
{
    atomic_init(&d->references, 0); atomic_init(&d->incarnation, i);
    d->task = task;
    d->retirement = retirement ? *retirement : (struct ss_owner_retirement){0};
    atomic_init(&d->admission, admitted);
}
void ss_owner_descriptor_init(struct ss_owner_descriptor *d, void *task, uint64_t i)
{ descriptor_init(d, task, i, true, NULL); }
void ss_owner_descriptor_init_closed(struct ss_owner_descriptor *d, uint64_t i,
                                     const struct ss_owner_retirement *retirement)
{ descriptor_init(d, NULL, i, false, retirement); }
static void notify_retirement(const struct ss_owner_descriptor *d)
{
    if (d->retirement.notify)
        d->retirement.notify(d->retirement.context, d->retirement.slot,
                             d->retirement.retirement_owner);
}
bool ss_owner_pin(struct ss_owner_descriptor *d, uint64_t incarnation,
                  const struct ss_owner_backend *backend)
{
    atomic_fetch_add_explicit(&d->references, 1, memory_order_acq_rel);
    ss_owner_barrier(backend);
    if (atomic_load_explicit(&d->admission, memory_order_acquire) &&
        atomic_load_explicit(&d->incarnation, memory_order_acquire) == incarnation)
        return true;
    ss_owner_put(d); return false;
}
void ss_owner_put(struct ss_owner_descriptor *d)
{
    if (atomic_fetch_sub_explicit(&d->references, 1, memory_order_release) != 1)
        return;
    /* Pair with close's admission-store/DMB/reference-load. Without this
     * store-to-load barrier both sides can miss the last retirement event. */
    ss_owner_barrier(NULL);
    if (!atomic_load_explicit(&d->admission, memory_order_acquire)) notify_retirement(d);
}
void ss_owner_close(struct ss_owner_descriptor *d, const struct ss_owner_backend *b)
{
    atomic_store_explicit(&d->admission, 0, memory_order_release);
    ss_owner_barrier(b);
    if (ss_owner_references_retired(d)) notify_retirement(d);
}
bool ss_owner_references_retired(const struct ss_owner_descriptor *d)
{ return atomic_load_explicit(&d->references, memory_order_acquire) == 0; }
bool ss_owner_reopen(struct ss_owner_descriptor *d, void *task, uint64_t i,
                     bool owners_drained)
{
    if (!owners_drained || atomic_load_explicit(&d->admission, memory_order_acquire)
        || !ss_owner_references_retired(d)) return false;
    d->task = task;
    atomic_store_explicit(&d->incarnation, i, memory_order_relaxed);
    atomic_store_explicit(&d->admission, 1, memory_order_release);
    return true;
}
