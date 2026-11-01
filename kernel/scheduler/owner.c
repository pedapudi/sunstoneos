/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "owner.h"
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
    for (unsigned p = 0; p < SS_OWNER_PRIORITIES; ++p)
        for (unsigned w = 0; w < 2; ++w) atomic_init(&o->incoming[p][w], 0);
    atomic_init(&o->dispatch, UINT64_C(1) << 3);
    atomic_init(&o->sgi_pending, 0);
    o->cpu = cpu; o->backend = backend;
}
bool ss_owner_enqueue(struct ss_owner *o, struct ss_owner_task *t, bool front)
{
    if (!t || t->queued || t == o->current || t->priority >= 8 || t->slot >= 128)
        return false;
    struct ss_owner_fifo *q = &o->ready[t->priority];
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
    struct ss_owner_fifo *q = &o->ready[t->priority];
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
    struct ss_owner_task *t = o->ready[p].head;
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
static void request_sgi(struct ss_owner *o)
{
    if (!atomic_exchange_explicit(&o->sgi_pending, 1, memory_order_acq_rel)) {
#if defined(__aarch64__)
        __asm__ volatile("dsb ishst" ::: "memory");
#endif
        if (o->backend && o->backend->sgi)
            o->backend->sgi(o->backend->context, o->cpu);
    }
}
void ss_owner_publish(struct ss_owner *o, unsigned slot, unsigned priority)
{
    if (slot >= 128 || priority >= 8) return;
    atomic_fetch_or_explicit(&o->incoming[priority][slot / 64],
                             UINT64_C(1) << (slot % 64), memory_order_release);
    ss_owner_barrier(o->backend);
    uint64_t status = atomic_load_explicit(&o->dispatch, memory_order_acquire);
    if ((status & 8) || priority > (status & 7)) request_sgi(o);
}
static void merge(struct ss_owner *o)
{
    for (unsigned p = 0; p < 8; ++p)
        for (unsigned w = 0; w < 2; ++w)
            o->detached[p][w] |= atomic_exchange_explicit(&o->incoming[p][w],
                                                          0, memory_order_acquire);
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
    for (; count < SS_OWNER_BATCH; ++count) {
        merge(o); /* Check fresh urgent work between every activation. */
        uint32_t priorities = 0;
        for (unsigned p = 0; p < 8; ++p)
            if (o->detached[p][0] | o->detached[p][1]) priorities |= 1u << p;
        if (!priorities) break;
        unsigned p = 31u - (unsigned)__builtin_clz(priorities);
        unsigned slot = next_slot(o, p);
        inspect(ctx, slot, p);
    }
    return count;
}
bool ss_owner_has_pending(const struct ss_owner *o)
{
    for (unsigned p = 0; p < 8; ++p)
        for (unsigned w = 0; w < 2; ++w)
            if (o->detached[p][w] || atomic_load_explicit(&o->incoming[p][w],
                                                       memory_order_acquire)) return true;
    return false;
}
bool ss_owner_commit_dispatch(struct ss_owner *o, unsigned priority, bool idle)
{
    uint64_t status = (++o->sequence << 4) | (priority & 7u) | (idle ? 8u : 0u);
    atomic_store_explicit(&o->dispatch, status, memory_order_release);
    atomic_store_explicit(&o->sgi_pending, 0, memory_order_release);
    ss_owner_barrier(o->backend);
    for (unsigned p = 0; p < 8; ++p) {
        if (!idle && p <= priority) continue;
        for (unsigned w = 0; w < 2; ++w)
            if (o->detached[p][w] || atomic_load_explicit(&o->incoming[p][w],
                                                       memory_order_acquire)) return true;
    }
    return false;
}
void ss_owner_continue(struct ss_owner *o)
{ if (ss_owner_has_pending(o)) request_sgi(o); }
void ss_owner_descriptor_init(struct ss_owner_descriptor *d, void *task, uint64_t i)
{
    atomic_init(&d->references, 0); atomic_init(&d->incarnation, i);
    d->task = task; atomic_init(&d->admission, 1);
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
{ atomic_fetch_sub_explicit(&d->references, 1, memory_order_release); }
void ss_owner_close(struct ss_owner_descriptor *d, const struct ss_owner_backend *b)
{
    atomic_store_explicit(&d->admission, 0, memory_order_release);
    ss_owner_barrier(b);
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
