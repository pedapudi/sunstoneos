/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "deadline.h"
#include <assert.h>
#include <stdio.h>
#include <stddef.h>
#include <threads.h>

_Static_assert(sizeof(struct ss_deadline_entry) == 48, "measurement entry layout");
_Static_assert(offsetof(struct ss_deadline_heap, published) == 1088, "measurement publication layout");

static struct ss_deadline_heap h, other;
static struct ss_deadline_entry e[129];
static uint64_t rng = 17;
static uint64_t random64(void)
{
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return rng;
}
static void reset(void)
{
    ss_deadline_init(&h); ss_deadline_init(&other);
    for (unsigned i = 0; i < 129; ++i) ss_deadline_entry_init(&e[i]);
}
static void invariant(void)
{
    uint64_t d = UINT64_MAX, local;
    for (unsigned i = 0; i < h.count; ++i) {
        struct ss_deadline_entry *a = h.entries[i];
        assert(a->owner == &h && a->index == i);
        if (i) {
            struct ss_deadline_entry *p = h.entries[(i - 1) / 2];
            assert(p->deadline < a->deadline ||
                   (p->deadline == a->deadline && p->order < a->order));
        }
        if (a->deadline < d) d = a->deadline;
    }
    assert(ss_deadline_next(&h, &local) == (h.count != 0));
    enum ss_deadline_state s = ss_deadline_snapshot(&h.published, &local);
    assert(s == (h.count ? SS_DEADLINE_VALUE : SS_DEADLINE_EMPTY));
    if (h.count) assert(local == d);
}
static void dense_and_cancel(void)
{
    reset();
    for (unsigned i = 0; i < 128; ++i) {
        assert(ss_deadline_insert(&h, &e[i], &e[i], UINT64_MAX-i, 128-i));
        invariant();
    }
    assert(!ss_deadline_insert(&h, &e[128], NULL, 0, 0));
    assert(!ss_deadline_insert(&other, &e[0], NULL, 0, 0));
    assert(!ss_deadline_cancel(&other, &e[0], UINT64_MAX));
    assert(!ss_deadline_cancel(&h, &e[0], UINT64_MAX-1));
    unsigned positions[] = {0, 39, 126};
    for (unsigned i = 0; i < 3; ++i) {
        struct ss_deadline_entry *a = h.entries[positions[i]];
        uint64_t gen = a->generation;
        assert(ss_deadline_cancel(&h, a, gen)); invariant();
        assert(!ss_deadline_cancel(&h, a, gen));
        assert(ss_deadline_insert(&h, a, a, gen+1, 0)); invariant();
        assert(!ss_deadline_cancel(&h, a, gen));
    }
    struct ss_deadline_result r;
    uint64_t prev = 0;
    for (unsigned i = 0; i < 128; ++i) {
        assert(ss_deadline_pop_expired(&h, UINT64_MAX, &r));
        assert(r.deadline >= prev); prev = r.deadline; invariant();
    }
    assert(!ss_deadline_pop_expired(&h, UINT64_MAX, &r));
}
static void ties(void)
{
    reset();
    for (unsigned i = 0; i < 128; ++i)
        assert(ss_deadline_insert(&h, &e[i], &e[i], i, 9));
    assert(ss_deadline_cancel(&h, &e[44], 44));
    assert(ss_deadline_insert(&h, &e[44], &e[44], 45, 9));
    struct ss_deadline_result r;
    assert(!ss_deadline_pop_expired(&h, 8, &r));
    for (unsigned i = 0; i < 128; ++i) {
        unsigned expected = i < 44 ? i : i < 127 ? i+1 : 44;
        assert(ss_deadline_pop_expired(&h, 9, &r));
        assert(r.context == &e[expected]);
    }
    /* Order exhaustion fails explicitly while existing ties remain ordered. */
    assert(ss_deadline_insert(&h, &e[0], &e[0], 0, 0));
    h.order = UINT64_MAX;
    assert(!ss_deadline_insert(&h, &e[1], &e[1], 0, 0));
    assert(ss_deadline_cancel(&h, &e[0], 0));
    assert(ss_deadline_insert(&h, &e[1], &e[1], 0, 0));
}
static void random_reference(void)
{
    reset();
    bool active[128] = {0};
    uint64_t deadline[128] = {0}, order[128] = {0}, gen[128] = {0}, serial=0;
    for (unsigned n = 0; n < 100000; ++n) {
        unsigned slot = (unsigned)(random64() % 128), op = (unsigned)(random64()%3);
        if (op == 0 && !active[slot]) {
            deadline[slot] = random64()%97; order[slot] = serial++;
            ++gen[slot];
            assert(ss_deadline_insert(&h, &e[slot], &e[slot], gen[slot], deadline[slot]));
            active[slot] = true;
        } else if (op == 1) {
            assert(ss_deadline_cancel(&h, &e[slot], gen[slot]) == active[slot]);
            active[slot] = false;
        } else {
            unsigned best = 128;
            for (unsigned i = 0; i < 128; ++i)
                if (active[i] && (best == 128 || deadline[i] < deadline[best] ||
                    (deadline[i] == deadline[best] && order[i] < order[best]))) best = i;
            uint64_t now = random64()%100;
            struct ss_deadline_result r;
            bool due = best != 128 && deadline[best] <= now;
            assert(ss_deadline_pop_expired(&h, now, &r) == due);
            if (due) {
                assert(r.context == &e[best] && r.generation == gen[best]);
                active[best] = false;
            }
        }
        invariant();
    }
}
struct callback_state { unsigned calls, wakes; uint64_t generation[128]; };
static void callback(void *context, const struct ss_deadline_result *r)
{
    struct callback_state *s = context;
    struct ss_deadline_entry *a = r->context;
    unsigned slot = (unsigned)(a - e);
    ++s->calls;
    if (s->generation[slot] == r->generation) ++s->wakes;
    if (slot == 0) {
        /* An in-flight timeout snapshot cannot cancel the reused wait. */
        assert(ss_deadline_insert(&h, a, a, r->generation+1, 999));
        assert(!ss_deadline_cancel(&h, a, r->generation));
        assert(ss_deadline_cancel(&h, &e[127], 127));
    }
}
static void bursts_and_generation(void)
{
    reset();
    struct callback_state s = {0};
    for (unsigned i = 0; i < 128; ++i) {
        s.generation[i] = i + (i == 50); /* stale object generation for slot50 */
        assert(ss_deadline_insert(&h, &e[i], &e[i], i, 5));
    }
    assert(ss_deadline_expire(&h, 100, 0, callback, &s) == 0);
    assert(ss_deadline_expire(&h, 100, 999, callback, &s) == 16);
    invariant();
    while (ss_deadline_expire(&h, 100, 16, callback, &s)) invariant();
    assert(s.calls == 127 && s.wakes == 126 && h.count == 1);
    assert(e[0].generation == 1 && e[0].deadline == 999);
}
static void eras_and_snapshot(void)
{
    uint64_t absolute;
    assert(ss_deadline_after(UINT32_MAX-3u, UINT32_MAX, &absolute));
    assert(absolute == UINT64_C(8589934587));
    assert(ss_deadline_after(UINT64_MAX-4, 4, &absolute) && absolute == UINT64_MAX);
    assert(!ss_deadline_after(UINT64_MAX-4, 5, &absolute));
    reset();
    assert(ss_deadline_insert(&h, &e[0], NULL, UINT64_MAX, UINT32_MAX+UINT64_C(1)));
    assert(ss_deadline_insert(&h, &e[1], NULL, 0, UINT64_MAX));
    struct ss_deadline_result r;
    assert(!ss_deadline_pop_expired(&h, UINT32_MAX, &r));
    assert(ss_deadline_pop_expired(&h, UINT32_MAX+UINT64_C(1), &r));
    assert(r.generation == UINT64_MAX);
    assert(ss_deadline_pop_expired(&h, UINT64_MAX, &r));
    atomic_store(&h.published.sequence, 3);
    assert(ss_deadline_snapshot(&h.published, &absolute) == SS_DEADLINE_UNKNOWN);
    atomic_store(&h.published.deadline, 123);
    atomic_store(&h.published.present, 1);
    assert(ss_deadline_snapshot(&h.published, &absolute) == SS_DEADLINE_UNKNOWN);
    atomic_store(&h.published.sequence, 4);
    assert(ss_deadline_snapshot(&h.published, &absolute) == SS_DEADLINE_VALUE);
    assert(absolute == 123);
}
/* Stress the exact publication ordering with a concurrent remote reader.
 * A valid publication is present only for even deadline values. */
static _Atomic bool writer_done;
static int publish_thread(void *argument)
{
    struct ss_deadline_publication *p = argument;
    for (uint64_t i = 1; i <= 200000; ++i) {
        atomic_store_explicit(&p->sequence, 2*i-1, memory_order_seq_cst);
        atomic_store_explicit(&p->deadline, i, memory_order_seq_cst);
        atomic_store_explicit(&p->present, !(i & 1u), memory_order_seq_cst);
        atomic_store_explicit(&p->sequence, 2*i, memory_order_seq_cst);
    }
    atomic_store(&writer_done, true);
    return 0;
}
static void concurrent_snapshot(void)
{
    reset();
    atomic_init(&writer_done, false);
    thrd_t writer;
    assert(thrd_create(&writer, publish_thread, &h.published) == thrd_success);
    do {
        uint64_t deadline = UINT64_MAX;
        enum ss_deadline_state state = ss_deadline_snapshot(&h.published, &deadline);
        if (state == SS_DEADLINE_VALUE) assert(!(deadline & 1u));
        else assert(deadline == UINT64_MAX);
    } while (!atomic_load(&writer_done));
    assert(thrd_join(writer, NULL) == thrd_success);
}
int main(void)
{
    dense_and_cancel(); ties(); random_reference();
    bursts_and_generation(); eras_and_snapshot(); concurrent_snapshot();
    puts("deadline: dense, ties, cancellation, reference order, batches, generations and eras pass");
}
