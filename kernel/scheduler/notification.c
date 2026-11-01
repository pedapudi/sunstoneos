/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "notification.h"

void ss_notification_init(struct ss_notification *n, uint32_t value)
{
    atomic_init(&n->word, value);
    atomic_init(&n->next_ticket, 0); atomic_init(&n->serving, 0);
}
enum ss_notification_state ss_notification_payload_state(uint64_t p)
{ return (enum ss_notification_state)((p >> 32) & 3); }
static uint64_t state(uint64_t p, enum ss_notification_state s)
{ return (p & ~SS_NOTIFICATION_STATE) | ((uint64_t)s << 32); }
static uint64_t transform(uint64_t p, enum ss_notification_action action,
                           uint32_t value, struct ss_notification_result *r)
{
    r->previous = (uint32_t)p;
    r->previous_state = ss_notification_payload_state(p);
    r->accepted = true; r->completed_wait = false;
    uint32_t v = (uint32_t)p;
    switch (action) {
    case SS_NOTIFY_INCREMENT: v++; break;
    case SS_NOTIFY_SET_BITS: v |= value; break;
    case SS_NOTIFY_OVERWRITE: v = value; break;
    case SS_NOTIFY_NO_OVERWRITE:
        if (r->previous_state == SS_NOTIFY_RECEIVED) {
            r->accepted = false; return p;
        }
        v = value; break;
    case SS_NOTIFY_NO_ACTION: break;
    case SS_NOTIFY_CLEAR_STATE:
        if (r->previous_state == SS_NOTIFY_RECEIVED)
            p = state(p, SS_NOTIFY_NOT_WAITING);
        return p;
    case SS_NOTIFY_CLEAR_BITS: return (p & ~SS_NOTIFICATION_VALUE) | (v & ~value);
    case SS_NOTIFY_QUERY: return p;
    }
    p = (p & ~SS_NOTIFICATION_VALUE) | v;
    if (r->previous_state == SS_NOTIFY_WAITING) {
        p |= SS_NOTIFICATION_COMPLETED; r->completed_wait = true;
    }
    return state(p, SS_NOTIFY_RECEIVED);
}
void ss_notification_gate_begin(struct ss_notification *n,
    const struct ss_owner_backend *backend, struct ss_notification_guard *g)
{
    /* Registration invalidates every older zero-count CAS snapshot. */
    atomic_fetch_add_explicit(&n->word, SS_NOTIFICATION_GATE_UNIT, memory_order_acq_rel);
    uint64_t ticket = atomic_fetch_add_explicit(&n->next_ticket, 1, memory_order_relaxed);
    while (atomic_load_explicit(&n->serving, memory_order_acquire) != ticket)
        ss_owner_wait(backend);
    g->slot = n; g->backend = backend; g->ticket = ticket;
    g->payload = atomic_load_explicit(&n->word, memory_order_acquire) & SS_NOTIFICATION_PAYLOAD;
}
void ss_notification_gate_end(struct ss_notification_guard *g)
{
    struct ss_notification *n = g->slot;
    /* Count changes from concurrent registrants must survive both writes. */
    atomic_fetch_and_explicit(&n->word, ~SS_NOTIFICATION_PAYLOAD, memory_order_relaxed);
    atomic_fetch_or_explicit(&n->word, g->payload & SS_NOTIFICATION_PAYLOAD,
                             memory_order_relaxed);
    atomic_fetch_sub_explicit(&n->word, SS_NOTIFICATION_GATE_UNIT, memory_order_release);
    atomic_store_explicit(&n->serving, g->ticket + 1, memory_order_release);
    ss_owner_event(g->backend);
}
struct ss_notification_result ss_notification_apply(struct ss_notification *n,
    enum ss_notification_action action, uint32_t value,
    const struct ss_owner_backend *backend)
{
    struct ss_notification_result r = {0};
    uint64_t old = atomic_load_explicit(&n->word, memory_order_acquire);
    if (!(old & SS_NOTIFICATION_GATE)) {
        uint64_t replacement = transform(old, action, value, &r);
        /* A no-overwrite refusal and a query linearize at this stable load. */
        if (!r.accepted || action == SS_NOTIFY_QUERY) return r;
        if (atomic_compare_exchange_strong_explicit(&n->word, &old, replacement,
                                      memory_order_acq_rel, memory_order_acquire)) return r;
    }
    struct ss_notification_guard g;
    ss_notification_gate_begin(n, backend, &g);
    g.payload = transform(g.payload, action, value, &r);
    r.used_gate = true;
    ss_notification_gate_end(&g);
    return r;
}
static bool condition(uint64_t payload, enum ss_notification_wait_kind kind)
{
    return kind == SS_NOTIFY_WAIT_COUNT ? (uint32_t)payload != 0 :
        ss_notification_payload_state(payload) == SS_NOTIFY_RECEIVED;
}
bool ss_notification_wait_enter(struct ss_notification_guard *g,
    struct ss_notification_wait *w, uint64_t generation,
    enum ss_notification_wait_kind kind, uint32_t clear_entry, bool may_block)
{
    g->payload &= ~SS_NOTIFICATION_COMPLETED;
    w->generation = generation; w->kind = kind; w->active = true;
    bool received = condition(g->payload, kind);
    if (!received) g->payload &= ~(uint64_t)clear_entry;
    w->blocked = !received && may_block;
    if (w->blocked) g->payload = state(g->payload, SS_NOTIFY_WAITING);
    return w->blocked;
}
bool ss_notification_wait_observe(struct ss_notification_guard *g,
    struct ss_notification_wait *w, uint64_t generation)
{
    if (!w->active || !w->blocked || w->generation != generation ||
        !(g->payload & SS_NOTIFICATION_COMPLETED)) return false;
    w->blocked = false;
    return true;
}
struct ss_notification_finish ss_notification_wait_finish(
    struct ss_notification_guard *g, struct ss_notification_wait *w,
    uint64_t generation, uint32_t clear_exit, bool decrement, bool timeout)
{
    struct ss_notification_finish r = {0};
    if (!w->active || w->generation != generation) return r;
    r.matched = true; r.value = (uint32_t)g->payload;
    r.condition = condition(g->payload, w->kind);
    r.scheduler_completed = (g->payload & SS_NOTIFICATION_COMPLETED) != 0;
    if (!timeout && !r.condition && !r.scheduler_completed) return r;
    if (r.condition) {
        if (w->kind == SS_NOTIFY_WAIT_COUNT) {
            uint32_t value = decrement ? r.value - 1u : 0;
            g->payload = (g->payload & ~SS_NOTIFICATION_VALUE) | value;
        } else g->payload &= ~(uint64_t)clear_exit;
    }
    g->payload = state(g->payload, SS_NOTIFY_NOT_WAITING);
    w->blocked = false; w->active = false;
    return r;
}
