/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "deadline.h"
#include <stddef.h>

_Static_assert(ATOMIC_LLONG_LOCK_FREE == 2, "deadline atomics must be lock-free");

static bool before(const struct ss_deadline_entry *a,
                   const struct ss_deadline_entry *b)
{
    return a->deadline < b->deadline ||
           (a->deadline == b->deadline && a->order < b->order);
}
static void place(struct ss_deadline_heap *h, unsigned i,
                  struct ss_deadline_entry *e)
{
    h->entries[i] = e;
    e->index = (uint16_t)i;
}
static void up(struct ss_deadline_heap *h, unsigned i,
               struct ss_deadline_entry *e)
{
    /* Index 127 has seven ancestors. */
    while (i != 0) {
        unsigned p = (i - 1u) / 2u;
        if (!before(e, h->entries[p])) break;
        place(h, i, h->entries[p]);
        i = p;
    }
    place(h, i, e);
}
static void down(struct ss_deadline_heap *h, unsigned i,
                 struct ss_deadline_entry *e)
{
    while (i * 2u + 1u < h->count) {
        unsigned c = i * 2u + 1u;
        if (c + 1u < h->count && before(h->entries[c + 1u], h->entries[c]))
            ++c;
        if (!before(h->entries[c], e)) break;
        place(h, i, h->entries[c]);
        i = c;
    }
    place(h, i, e);
}
static void publish(struct ss_deadline_heap *h)
{
    /* Sequentially consistent atomics give one total order across all three
     * fields, including readers. No ordinary-data seqlock race is present. */
    uint64_t s = atomic_load_explicit(&h->published.sequence, memory_order_seq_cst);
    atomic_store_explicit(&h->published.sequence, s + 1u, memory_order_seq_cst);
    atomic_store_explicit(&h->published.deadline,
                         h->count ? h->entries[0]->deadline : 0,
                         memory_order_seq_cst);
    atomic_store_explicit(&h->published.present, h->count != 0,
                         memory_order_seq_cst);
    atomic_store_explicit(&h->published.sequence, s + 2u, memory_order_seq_cst);
}
void ss_deadline_init(struct ss_deadline_heap *h)
{
    h->count = 0;
    h->order = 0;
    atomic_init(&h->published.sequence, 0);
    atomic_init(&h->published.deadline, 0);
    atomic_init(&h->published.present, 0);
}
void ss_deadline_entry_init(struct ss_deadline_entry *e)
{
    *e = (struct ss_deadline_entry){ .index = SS_DEADLINE_NONE };
}
bool ss_deadline_insert(struct ss_deadline_heap *h, struct ss_deadline_entry *e,
                        void *context, uint64_t generation, uint64_t deadline)
{
    if (e->owner != NULL || e->index != SS_DEADLINE_NONE ||
        h->count == SS_DEADLINE_CAPACITY) return false;
    /* Reset insertion order only when no existing tie can be reordered. */
    if (h->count == 0) h->order = 0;
    if (h->order == UINT64_MAX) return false;
    e->context = context;
    e->generation = generation;
    e->deadline = deadline;
    e->order = h->order++;
    e->owner = h;
    up(h, h->count++, e);
    publish(h);
    return true;
}
static void remove_entry(struct ss_deadline_heap *h, struct ss_deadline_entry *e)
{
    unsigned i = e->index;
    struct ss_deadline_entry *last = h->entries[--h->count];
    if (i < h->count) {
        if (i && before(last, h->entries[(i - 1u) / 2u])) up(h, i, last);
        else down(h, i, last);
    }
    h->entries[h->count] = NULL;
    e->owner = NULL;
    e->index = SS_DEADLINE_NONE;
    publish(h);
}
bool ss_deadline_cancel(struct ss_deadline_heap *h, struct ss_deadline_entry *e,
                        uint64_t generation)
{
    if (e->owner != h || e->index >= h->count ||
        h->entries[e->index] != e || e->generation != generation) return false;
    remove_entry(h, e);
    return true;
}
bool ss_deadline_pop_expired(struct ss_deadline_heap *h, uint64_t now,
                             struct ss_deadline_result *out)
{
    if (!h->count || h->entries[0]->deadline > now) return false;
    struct ss_deadline_entry *e = h->entries[0];
    *out = (struct ss_deadline_result){e->context, e->generation, e->deadline};
    remove_entry(h, e);
    return true;
}
unsigned ss_deadline_expire(struct ss_deadline_heap *h, uint64_t now,
                            unsigned budget, ss_deadline_expired callback, void *ctx)
{
    unsigned n = 0;
    struct ss_deadline_result r;
    if (budget > SS_DEADLINE_BATCH) budget = SS_DEADLINE_BATCH;
    while (n < budget && ss_deadline_pop_expired(h, now, &r)) {
        ++n;
        callback(ctx, &r);
    }
    return n;
}
bool ss_deadline_next(const struct ss_deadline_heap *h, uint64_t *out)
{
    if (!h->count) return false;
    *out = h->entries[0]->deadline;
    return true;
}
enum ss_deadline_state ss_deadline_snapshot(
    const struct ss_deadline_publication *p, uint64_t *out)
{
    uint64_t a = atomic_load_explicit(&p->sequence, memory_order_seq_cst);
    if (a & 1u) return SS_DEADLINE_UNKNOWN;
    uint64_t d = atomic_load_explicit(&p->deadline, memory_order_seq_cst);
    uint64_t present = atomic_load_explicit(&p->present, memory_order_seq_cst);
    uint64_t b = atomic_load_explicit(&p->sequence, memory_order_seq_cst);
    if (a != b) return SS_DEADLINE_UNKNOWN;
    if (!present) return SS_DEADLINE_EMPTY;
    *out = d;
    return SS_DEADLINE_VALUE;
}
bool ss_deadline_after(uint64_t now, uint64_t delta, uint64_t *absolute)
{
    if (delta > UINT64_MAX - now) return false;
    *absolute = now + delta;
    return true;
}
