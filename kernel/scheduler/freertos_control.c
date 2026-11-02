/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "freertos_control.h"
#include <stddef.h>

#define ROUTE_CLOSED (UINT64_C(1) << 63)
#define ROUTE_COUNT (ROUTE_CLOSED - 1)
#define ROUTE_SHIFT 8u

static void lock(struct ss_control *c, struct ss_control_slot *s)
{
    uint32_t t = atomic_fetch_add_explicit(&s->tickets, 1, memory_order_relaxed);
    while (atomic_load_explicit(&s->serving, memory_order_acquire) != t)
        ss_owner_wait(c->backend);
}
static void unlock(struct ss_control *c, struct ss_control_slot *s)
{
    atomic_fetch_add_explicit(&s->serving, 1, memory_order_release);
    ss_owner_event(c->backend);
}
static void hint(struct ss_control *c, unsigned cpu, unsigned slot)
{
    atomic_fetch_or_explicit(&c->pending[cpu][slot / 64], UINT64_C(1) << (slot % 64), memory_order_release);
    ss_owner_barrier(c->backend);
    if (c->backend && c->backend->sgi) c->backend->sgi(c->backend->context, cpu);
}
static void all(struct ss_control *c, unsigned slot)
{
    for (unsigned cpu = 0; cpu < SS_CONTROL_CPUS; ++cpu)
        if (c->active_mask & (UINT64_C(1) << cpu)) hint(c, cpu, slot);
}
bool ss_control_route_pin(struct ss_control *c, unsigned slot,
                           struct ss_control_route *out)
{
    *out = (struct ss_control_route){0};
    struct ss_control_slot *s = &c->slot[slot];
    uint64_t word = atomic_load_explicit(&s->route_word, memory_order_acquire);
#ifdef SS_CONTROL_ROUTE_TEST
    extern void ss_control_route_test_sample(uint64_t);
    ss_control_route_test_sample(word);
#endif
    uint64_t epoch = word >> ROUTE_SHIFT;
    uint64_t previous = atomic_fetch_add_explicit(&s->route_readers[epoch & 1],
                                                 1, memory_order_acq_rel);
    ss_owner_barrier(c->backend);
    struct ss_control_route route = { c, epoch, slot, (unsigned)(word & 0xff) };
    if (!(previous & ROUTE_CLOSED) &&
        atomic_load_explicit(&s->route_word, memory_order_acquire) == word) {
        *out = route;
        return true;
    }
    ss_control_route_put(&route);
    return false;
}
void ss_control_route_put(struct ss_control_route *route)
{
    if (!route->control) return;
    struct ss_control *c = route->control;
    struct ss_control_slot *s = &c->slot[route->slot];
    uint64_t previous = atomic_fetch_sub_explicit(&s->route_readers[route->epoch & 1],
                                                 1, memory_order_release);
    /* Close and participant release use the same atomic word. Either close
     * observes zero or the final closed participant publishes this event. */
    if ((previous & ROUTE_CLOSED) && (previous & ROUTE_COUNT) == 1)
        all(c, route->slot);
    *route = (struct ss_control_route){0};
}
static uint64_t route_close(struct ss_control_slot *s)
{
    uint64_t epoch = atomic_load_explicit(&s->route_word, memory_order_acquire) >> ROUTE_SHIFT;
    atomic_fetch_or_explicit(&s->route_readers[epoch & 1], ROUTE_CLOSED, memory_order_acq_rel);
    return epoch;
}
static bool route_drained(struct ss_control_slot *s, uint64_t epoch)
{
    return atomic_load_explicit(&s->route_readers[epoch & 1], memory_order_acquire) == ROUTE_CLOSED;
}
static void route_adopt(struct ss_control_slot *s)
{
    uint64_t next = s->transfer.route_epoch + 1;
    /* Epochs must not wrap. Preserve transient rejected admissions to a closed
     * bank: resetting its count would lose their subsequent decrements. The
     * release reopening orders the intervening route before a stale reader's
     * acquiring registration, so two-bank reuse cannot validate an old word. */
    atomic_fetch_and_explicit(&s->route_readers[next & 1], ROUTE_COUNT, memory_order_release);
    atomic_store_explicit(&s->route_word,
        (next << ROUTE_SHIFT) | s->transfer.new_owner, memory_order_release);
}
void ss_control_init(struct ss_control *c, uint64_t active,
                     const struct ss_owner_backend *b, const struct ss_control_ops *ops)
{
    *c = (struct ss_control){ .backend = b, .ops = *ops, .active_mask = active };
}
void ss_control_register(struct ss_control *c, unsigned slot,
                         struct ss_owner_descriptor *d, uint64_t incarnation,
                         unsigned owner, unsigned priority, uint64_t affinity)
{
    struct ss_control_slot *s = &c->slot[slot];
    /* A closed, unreferenced slot can still have a retirement consumer queued
     * on this permanent FIFO. Never reset its ticket counters on reuse. */
    lock(c, s);
    s->descriptor = d;
    s->desired = (struct ss_control_state){ .incarnation = incarnation,
        .priority = priority, .effective_priority = priority, .affinity = affinity };
    s->transfer = (struct ss_control_transfer){0};
    atomic_store_explicit(&s->owner, owner, memory_order_release);
    /* Registry registration requires lifetime references and all old owner
     * hints retired. No valid routing participant survives reincarnation. */
    atomic_store_explicit(&s->route_readers[0], 0, memory_order_relaxed);
    atomic_store_explicit(&s->route_readers[1], ROUTE_CLOSED, memory_order_relaxed);
    atomic_store_explicit(&s->route_word, owner, memory_order_release);
    atomic_store_explicit(&s->applied, 0, memory_order_release);
    s->phase = 0;
    s->retained = false;
    s->old_drained = false;
    s->drain_mask = 0;
    unlock(c, s);
}
uint64_t ss_control_publish(struct ss_control *c, unsigned slot,
                            ss_control_mutate mutate, void *context)
{
    struct ss_control_slot *s = &c->slot[slot];
    lock(c, s);
    bool was_suspended = s->desired.suspended;
    if (!s->desired.dead && mutate(context, &s->desired)) {
        if (!s->retained) {
            atomic_fetch_add_explicit(&s->descriptor->references, 1, memory_order_relaxed);
            s->retained = true;
        }
        ++s->desired.sequence;
        if (!was_suspended && s->desired.suspended)
            s->desired.offcpu_sequence = s->desired.sequence;
        /* A deletion's close and logical object detach are mutation duties. */
        hint(c, s->owner, slot);
    }
    uint64_t seq = s->desired.sequence;
    unlock(c, s);
    return seq;
}
void ss_control_snapshot(struct ss_control *c, unsigned slot, struct ss_control_state *out)
{
    lock(c, &c->slot[slot]); *out = c->slot[slot].desired; unlock(c, &c->slot[slot]);
}
bool ss_control_applied(const struct ss_control *c, unsigned slot, uint64_t seq)
{
    return atomic_load_explicit(&c->slot[slot].applied, memory_order_acquire) >= seq;
}
void ss_control_wait(struct ss_control *c, unsigned slot, uint64_t seq)
{
    while (!ss_control_applied(c, slot, seq)) ss_owner_wait(c->backend);
}
void ss_control_repair(struct ss_control *c, unsigned slot)
{ hint(c, atomic_load_explicit(&c->slot[slot].owner, memory_order_acquire), slot); }
void ss_control_forward(struct ss_control *c, unsigned slot)
{ ss_control_repair(c, slot); }
void ss_control_retirement(struct ss_control *c, unsigned slot) { all(c, slot); }
static unsigned destination(uint64_t mask)
{
    return mask ? (unsigned)__builtin_ctzll(mask) : SS_CONTROL_NONE;
}
static void consume(struct ss_control *c, unsigned cpu, unsigned slot)
{
    struct ss_control_slot *s = &c->slot[slot];
    bool put = false, retire = false;
    uint64_t incarnation;
    lock(c, s);
    incarnation = s->desired.incarnation;
    if (!s->desired.dead && !s->retained && cpu == s->owner &&
        ss_owner_pin(s->descriptor, s->desired.incarnation, c->backend)) {
        s->retained = true;
    }
    if (s->phase == 3 && cpu == s->transfer.old_owner &&
        route_drained(s, s->transfer.route_epoch)) {
        if (!c->ops.drained || c->ops.drained(c->ops.context, cpu, slot)) {
            s->old_drained = true;
            hint(c, s->owner, slot);
        } else {
            /* This entry will drain the local activation batch after controls.
             * Arrange a fresh checkpoint after that work, not an early ack. */
            hint(c, cpu, slot);
        }
    }
    if (s->desired.dead && !s->retained) {
        /* A live lifetime pin can belong to the interrupted delete caller.
         * Reissuing an SGI here would prevent that caller from returning to
         * release it. Close/last-put already publishes retirement to every
         * owner, so only local activation drainage needs another checkpoint. */
        if (ss_owner_references_retired(s->descriptor)) {
            if (!c->ops.drained || c->ops.drained(c->ops.context, cpu, slot)) {
                s->drain_mask |= UINT64_C(1) << cpu;
                retire = s->drain_mask == c->active_mask;
            } else hint(c, cpu, slot);
        }
    } else if (cpu == s->owner && s->retained) {
        void *task = s->descriptor->task; /* retained reference predates close */
        if (s->phase == 2) {
            /* Detach has acknowledged the saved frame. Adopt before applying
             * even a deletion that arrived during transfer. */
            c->ops.adopt(c->ops.context, slot, task, &s->transfer);
            route_adopt(s);
            s->phase = 3; hint(c, s->transfer.old_owner, slot);
        }
        if (s->desired.dead) {
            (void)route_close(s);
            if (!c->ops.apply(c->ops.context, slot, task, &s->desired)) {
                if (!c->ops.deferred || !c->ops.deferred(c->ops.context, slot)) hint(c, cpu, slot);
                unlock(c, s); return;
            }
            atomic_store_explicit(&s->applied, s->desired.sequence, memory_order_release);
            s->phase = 0; s->retained = false; put = true;
            all(c, slot);
        } else {
            if (s->phase == 0 && !(s->desired.affinity & (UINT64_C(1) << cpu))) {
                unsigned next = destination(s->desired.affinity & c->active_mask);
                if (next != SS_CONTROL_NONE) {
                    s->transfer.old_owner = cpu; s->transfer.new_owner = next;
                    s->transfer.route_epoch = route_close(s);
                    ++s->transfer.epoch; s->old_drained = false; s->phase = 1;
                }
            }
            if (s->phase == 1) {
                if (c->ops.detach(c->ops.context, slot, task, &s->transfer)) {
                    s->phase = 2; s->owner = s->transfer.new_owner;
                    hint(c, s->owner, slot);
                } else hint(c, cpu, slot);
            } else {
                bool completed = c->ops.apply(c->ops.context, slot, task, &s->desired);
                bool eligible = (s->desired.affinity & (UINT64_C(1) << cpu)) != 0;
                if (completed && eligible) {
                    s->desired.offcpu_sequence = 0;
                    atomic_store_explicit(&s->applied, s->desired.sequence, memory_order_release);
                }
                else if (!c->ops.deferred || !c->ops.deferred(c->ops.context, slot)) hint(c, cpu, slot);
                if (s->phase == 3) {
                    /* The acknowledgment follows old-epoch closure, reader
                     * retirement and a fresh owner drain. New-route publishers
                     * and unrelated lifetime pins do not postpone retirement. */
                    if (s->old_drained) s->phase = 0;
                }
                if (!s->phase && completed && eligible) { s->retained = false; put = true; }
                else if (!eligible) hint(c, cpu, slot);
            }
        }
    } else hint(c, s->owner, slot);
    unlock(c, s);
    /* A last-put callback may publish retirement; never invoke under lock. */
    if (put) ss_owner_put(s->descriptor);
    if (retire && c->ops.retired) c->ops.retired(c->ops.context, slot, incarnation);
    ss_owner_event(c->backend);
}
unsigned ss_control_drain(struct ss_control *c, unsigned cpu)
{
    unsigned count = 0, start = c->cursor[cpu];
    for (unsigned offset = 0; offset < SS_OWNER_SLOTS && count < SS_OWNER_BATCH; ++offset) {
        unsigned slot = (start + offset) % SS_OWNER_SLOTS;
        uint64_t bit = UINT64_C(1) << (slot % 64);
        if (!(atomic_load_explicit(&c->pending[cpu][slot / 64], memory_order_acquire) & bit)) continue;
        if (atomic_fetch_and_explicit(&c->pending[cpu][slot / 64], ~bit, memory_order_acquire) & bit) {
            consume(c, cpu, slot); ++count;
            c->cursor[cpu] = (slot + 1) % SS_OWNER_SLOTS;
        }
    }
    return count;
}
bool ss_control_checkpoint(struct ss_control *c, unsigned cpu)
{
    unsigned phase = atomic_load_explicit(&c->gate_phase, memory_order_acquire);
    if (phase == SS_GATE_ACQUIRING || phase == SS_GATE_HELD) {
        atomic_fetch_or_explicit(&c->gate_ack, UINT64_C(1) << cpu, memory_order_release);
        ss_owner_event(c->backend); return true;
    }
    return false;
}
void ss_control_suspend_all(struct ss_control *c, unsigned cpu, uintptr_t holder,
                            uint64_t incarnation, bool running)
{
    if (!running) { ++c->boot_depth; return; }
    if (atomic_load_explicit(&c->gate_phase, memory_order_acquire) == SS_GATE_HELD &&
        c->gate_holder == holder && c->gate_incarnation == incarnation) { ++c->gate_depth; return; }
    uint32_t ticket = atomic_fetch_add_explicit(&c->gate_tickets, 1, memory_order_relaxed);
    while (atomic_load_explicit(&c->gate_serving, memory_order_acquire) != ticket) ss_owner_wait(c->backend);
    c->gate_holder = holder; c->gate_incarnation = incarnation; c->gate_depth = 1;
    atomic_store_explicit(&c->gate_ack, 0, memory_order_relaxed);
    atomic_fetch_add_explicit(&c->gate_epoch, 1, memory_order_relaxed);
    atomic_store_explicit(&c->gate_phase, SS_GATE_ACQUIRING, memory_order_release);
    for (unsigned target = 0; target < SS_CONTROL_CPUS; ++target)
        if ((c->active_mask & (UINT64_C(1) << target)) && c->backend && c->backend->sgi)
            c->backend->sgi(c->backend->context, target);
    (void)ss_control_checkpoint(c, cpu);
    while (atomic_load_explicit(&c->gate_ack, memory_order_acquire) != c->active_mask) ss_owner_wait(c->backend);
    atomic_store_explicit(&c->gate_phase, SS_GATE_HELD, memory_order_release);
}
bool ss_control_resume_all(struct ss_control *c, unsigned cpu, uintptr_t holder,
                           uint64_t incarnation, bool running)
{
    if (!running) { if (c->boot_depth) --c->boot_depth; return false; }
    if (atomic_load_explicit(&c->gate_phase, memory_order_acquire) != SS_GATE_HELD ||
        c->gate_holder != holder || c->gate_incarnation != incarnation) return false;
    if (--c->gate_depth) return false;
    atomic_store_explicit(&c->gate_phase, SS_GATE_RELEASING, memory_order_release);
    bool yielded = c->ops.dispatch && c->ops.dispatch(c->ops.context, cpu);
    for (unsigned target = 0; target < SS_CONTROL_CPUS; ++target)
        if ((c->active_mask & (UINT64_C(1) << target)) && c->backend && c->backend->sgi)
            c->backend->sgi(c->backend->context, target);
    atomic_store_explicit(&c->gate_phase, SS_GATE_OPEN, memory_order_release);
    atomic_fetch_add_explicit(&c->gate_serving, 1, memory_order_release);
    ss_owner_event(c->backend); return yielded;
}
