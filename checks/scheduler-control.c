/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../kernel/scheduler/freertos_control.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
static struct ss_control control;
static struct ss_owner_descriptor descriptor[SS_OWNER_SLOTS];
static _Atomic unsigned publications, applied, adopted, retired;
static _Atomic bool offcpu;
static bool apply(void *context, unsigned slot, void *task, const struct ss_control_state *state)
{
    (void)context; assert(task == &descriptor[slot]); assert(state->incarnation == 1);
    atomic_fetch_add(&applied, 1);
    return !state->offcpu_sequence || atomic_load(&offcpu);
}
static bool detach(void *context, unsigned slot, void *task, struct ss_control_transfer *transfer)
{
    (void)context; (void)slot; (void)task;
    transfer->deadline = 197; transfer->wait_generation = 31; transfer->wait_outcome = 7;
    return atomic_load(&offcpu);
}
static void adopt(void *context, unsigned slot, void *task, const struct ss_control_transfer *transfer)
{
    (void)context; (void)slot; (void)task;
    assert(transfer->deadline == 197 && transfer->wait_generation == 31 && transfer->wait_outcome == 7);
    atomic_fetch_add(&adopted, 1);
}
static bool drained(void *context, unsigned cpu, unsigned slot)
{ (void)context; (void)cpu; (void)slot; return true; }
static void retirement(void *context, unsigned slot, uint64_t incarnation)
{ (void)context; (void)slot; assert(incarnation == 1); atomic_fetch_add(&retired, 1); }
static void wait_event(void *context)
{
    (void)context;
    for (unsigned cpu = 0; cpu < 2; ++cpu) ss_control_checkpoint(&control, cpu);
    sched_yield();
}
static bool dispatch(void *context, unsigned cpu) { (void)context; (void)cpu; return true; }
static bool increment(void *context, struct ss_control_state *state)
{ (void)context; ++state->resume_generation; return true; }
static bool migrate(void *context, struct ss_control_state *state)
{ (void)context; state->affinity = 2; return true; }
static bool suspend_task(void *context, struct ss_control_state *state)
{ (void)context; state->suspended = true; return true; }
static bool resume_task(void *context, struct ss_control_state *state)
{ (void)context; if (!state->suspended) return false; state->suspended = false; ++state->resume_generation; return true; }
static bool delete_task(void *context, struct ss_control_state *state)
{ ss_owner_close(context, NULL); state->dead = true; return true; }
static void *publisher(void *context)
{
    unsigned slot = (unsigned)(uintptr_t)context;
    for (unsigned i = 0; i < 10000; ++i) {
        assert(ss_owner_pin(&descriptor[slot], 1, NULL));
        ss_control_publish(&control, slot, increment, NULL);
        ss_owner_put(&descriptor[slot]);
    }
    atomic_fetch_add(&publications, 1); return NULL;
}
static void *gate_caller(void *context)
{
    unsigned cpu = (unsigned)(uintptr_t)context;
    for (unsigned i = 0; i < 1000; ++i) {
        ss_control_suspend_all(&control, cpu, cpu + 1, 1, true);
        ss_control_suspend_all(&control, cpu, cpu + 1, 1, true);
        assert(!ss_control_resume_all(&control, cpu, cpu + 1, 1, true));
        assert(ss_control_checkpoint(&control, cpu));
        assert(ss_control_resume_all(&control, cpu, cpu + 1, 1, true));
    }
    return NULL;
}
/* The owners' empty check, as the kernel makes it (ssKernelControlDue): only
 * the inbox line while its request count equals the one last checked. Every
 * drain below goes through it, so a path that makes a request due without
 * counting it leaves that request unapplied and fails the later assertions. */
static struct ss_owner_inbox inbox[2];
static uint32_t checked[2];
static unsigned skipped;
static bool due(unsigned cpu)
{
    unsigned gate = atomic_load_explicit(&inbox[cpu].gate, memory_order_acquire);
    if (gate == SS_GATE_ACQUIRING || gate == SS_GATE_HELD) return true;
    uint32_t requests = atomic_load_explicit(&inbox[cpu].control, memory_order_acquire);
    if (requests == checked[cpu]) { ++skipped; return false; }
    if (ss_control_has_pending(&control, cpu)) return true;
    checked[cpu] = requests;
    return false;
}
static void drain(void)
{
    for (unsigned n = 0; n < 32; ++n)
        for (unsigned cpu = 0; cpu < 2; ++cpu)
            if (due(cpu)) ss_control_drain(&control, cpu);
}
/* Each path that makes a request pending and not parked counts it once;
 * parking alone does not. */
static void request_counts(void)
{
    const unsigned slot = 9;
    uint32_t before = atomic_load(&inbox[1].control);
    ss_control_park(&control, 1, slot);
    assert(atomic_load(&inbox[1].control) == before && !ss_control_has_pending(&control, 1));
    ss_control_wake(&control, 1, slot);
    assert(atomic_load(&inbox[1].control) == before + 1 && ss_control_has_pending(&control, 1));
    assert(ss_control_drain_slot(&control, 1, slot) == 1);
    ss_control_park(&control, 1, slot);
    const uint64_t slots[2] = { UINT64_C(1) << slot, 0 };
    ss_control_wake_mask(&control, 1, slots, &control.slot[slot].serving);
    assert(atomic_load(&inbox[1].control) == before + 2 && ss_control_has_pending(&control, 1));
    const uint64_t none[2] = {0};
    ss_control_wake_mask(&control, 1, none, &control.slot[slot].serving);
    assert(atomic_load(&inbox[1].control) == before + 2);
    assert(ss_control_drain_slot(&control, 1, slot) == 1);
    assert(!ss_control_has_pending(&control, 1));
}
/* Negative control: reproduce the old command path's general drain while
 * retaining the assertions about the one-request contract. */
#ifdef SS_CONTROL_DRAIN_ALL_NEGATIVE
#define ss_control_drain_slot(c, cpu, slot) ((void)(slot), ss_control_drain(c, cpu))
#endif
static void single_request_and_remaining_batch(void)
{
    const unsigned target = 2, other = 3;
    const uint64_t target_bit = UINT64_C(1) << target, other_bit = UINT64_C(1) << other;
    control.cursor[0].value = SS_OWNER_SLOTS - 1;
    unsigned cursor = control.cursor[0].value;
    uint64_t requested = ss_control_publish(&control, target, suspend_task, NULL);
    uint64_t unrelated = ss_control_publish(&control, other, increment, NULL);
    unsigned before = atomic_load(&applied);
    assert(ss_control_drain_slot(&control, 0, target) == 1);
    assert(atomic_load(&applied) == before + 1 && control.cursor[0].value == cursor);
    assert(!ss_control_applied(&control, target, requested));
    assert(!ss_control_applied(&control, other, unrelated));
    /* The running target republishes its off-CPU fence; the unrelated hint
     * remains available to the next maintenance point. */
    assert((atomic_load(&control.pending[0].word[0]) & (target_bit | other_bit)) == (target_bit | other_bit));
    assert(ss_control_has_pending(&control, 0));
    atomic_store(&offcpu, true);
    assert(ss_control_drain_slot(&control, 0, target) == 1);
    assert(ss_control_applied(&control, target, requested));
    assert(!ss_control_applied(&control, other, unrelated));
    assert(control.cursor[0].value == cursor);
    assert(ss_control_drain_slot(&control, 0, target) == 0);
    assert(ss_control_drain(&control, 0) == 1);
    assert(ss_control_applied(&control, other, unrelated));
    assert(!ss_control_has_pending(&control, 0));
    ss_control_publish(&control, target, resume_task, NULL);
    drain();
    atomic_store(&offcpu, false);

    /* More than one batch spans both bitmap words and wraps the cursor. The
     * owner must request another entry even when no wake or timeout remains. */
    control.cursor[0].value = SS_OWNER_SLOTS - 1;
    for (unsigned slot = 60; slot < 60 + SS_OWNER_BATCH + 1; ++slot)
        ss_control_publish(&control, slot, increment, NULL);
    uint64_t visited[2] = {0};
    assert(ss_control_drain_excluding(&control, 0, visited) == SS_OWNER_BATCH);
    assert(ss_control_has_pending(&control, 0));
    assert(control.cursor[0].value == 60 + SS_OWNER_BATCH);
    assert(ss_control_drain_excluding(&control, 0, visited) == 0 && "one maintenance epoch has one total batch budget");
    assert(ss_control_drain(&control, 0) == 1);
    assert(!ss_control_has_pending(&control, 0));
}
int main(void)
{
    struct ss_owner_backend backend = { .wait = wait_event };
    struct ss_control_ops ops = { .apply = apply, .detach = detach, .adopt = adopt,
        .drained = drained, .retired = retirement, .dispatch = dispatch };
    ss_control_init(&control, 3, &backend, &ops);
    for (unsigned cpu = 0; cpu < 2; ++cpu) ss_control_attach_inbox(&control, cpu, &inbox[cpu]);
    for (unsigned slot = 0; slot < SS_OWNER_SLOTS; ++slot) {
        ss_owner_descriptor_init(&descriptor[slot], &descriptor[slot], 1);
        ss_control_register(&control, slot, &descriptor[slot], 1, 0, 1, 3);
    }
    single_request_and_remaining_batch();
    request_counts();
    pthread_t threads[2];
    for (unsigned n = 0; n < 2; ++n) assert(!pthread_create(&threads[n], NULL, publisher, (void *)(uintptr_t)n));
    while (atomic_load(&publications) < 2) drain();
    for (unsigned n = 0; n < 2; ++n) assert(!pthread_join(threads[n], NULL));
    drain();
    for (unsigned n = 0; n < 2; ++n) {
        struct ss_control_state state; ss_control_snapshot(&control, n, &state);
        assert(state.resume_generation == 10000 && ss_control_applied(&control, n, 10000));
    }
    uint64_t seq = ss_control_publish(&control, 0, suspend_task, NULL);
    drain(); assert(!ss_control_applied(&control, 0, seq));
    atomic_store(&offcpu, true); drain(); assert(ss_control_applied(&control, 0, seq));
    seq = ss_control_publish(&control, 0, resume_task, NULL);
    assert(ss_control_publish(&control, 0, resume_task, NULL) == seq); drain();
    atomic_store(&offcpu, false);
    seq = ss_control_publish(&control, 1, suspend_task, NULL);
    seq = ss_control_publish(&control, 1, resume_task, NULL);
    drain(); assert(!ss_control_applied(&control, 1, seq));
    atomic_store(&offcpu, true); drain(); assert(ss_control_applied(&control, 1, seq));
    atomic_store(&offcpu, false);
    assert(ss_owner_pin(&descriptor[0], 1, NULL)); /* late old publisher */
    struct ss_control_route old_route, new_route;
    assert(ss_control_route_pin(&control, 0, &old_route));
    assert(old_route.owner == 0);
    seq = ss_control_publish(&control, 0, migrate, NULL);
    drain(); assert(!ss_control_applied(&control, 0, seq) && !atomic_load(&adopted));
    atomic_store(&offcpu, true); drain();
    assert(atomic_load(&adopted) == 1 && control.slot[0].phase == 3);
    assert(ss_control_route_pin(&control, 0, &new_route));
    assert(new_route.owner == 1 && new_route.epoch != old_route.epoch);
    ss_control_forward(&control, 0); drain();
    ss_control_route_put(&old_route);
    drain(); assert(control.slot[0].phase == 0);
    /* A current-epoch publisher and lifetime pin do not hold old forwarding. */
    ss_control_route_put(&new_route);
    ss_owner_put(&descriptor[0]);
    assert(ss_owner_pin(&descriptor[0], 1, NULL));
    ss_control_publish(&control, 0, delete_task, &descriptor[0]); drain();
    assert(!atomic_load(&retired));
    ss_owner_put(&descriptor[0]); ss_control_retirement(&control, 0); drain();
    assert(atomic_load(&retired));
    for (unsigned n = 0; n < 2; ++n) assert(!pthread_create(&threads[n], NULL, gate_caller, (void *)(uintptr_t)n));
    for (unsigned n = 0; n < 2; ++n) assert(!pthread_join(threads[n], NULL));
    assert(atomic_load(&control.gate_phase) == SS_GATE_OPEN);
    for (unsigned cpu = 0; cpu < 2; ++cpu) assert(atomic_load(&inbox[cpu].gate) == SS_GATE_OPEN);
    assert(skipped && "most empty checks read only the inbox");
    puts("scheduler control: one-request drain, remaining-batch continuation, concurrent publication, off-CPU suspend/transfer, late publisher, retirement and FIFO nested gate passed");
    return 0;
}
