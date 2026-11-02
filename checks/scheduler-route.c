/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../kernel/scheduler/freertos_control.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>

static struct ss_control control;
static struct ss_owner run[3];
static struct ss_owner_descriptor descriptor;
static _Atomic unsigned interrupts[3], sampled, proceed;
static _Thread_local bool pause_sample;
static unsigned home, current_cpu, adoptions, retirements, inspected[3];
static unsigned task_storage;
static void sgi(void *context, unsigned cpu)
{ (void)context; assert(cpu < 3); atomic_fetch_add(&interrupts[cpu], 1); }
static const struct ss_owner_backend backend = { .sgi = sgi };
void ss_control_route_test_sample(uint64_t word)
{
    if (!pause_sample) return;
    assert(word == 0);
    atomic_store_explicit(&sampled, 1, memory_order_release);
    while (!atomic_load_explicit(&proceed, memory_order_acquire)) sched_yield();
}
static bool apply(void *context, unsigned slot, void *task, const struct ss_control_state *state)
{ (void)context; (void)state; assert(slot == 0 && task == &task_storage && home == current_cpu); return true; }
static bool detach(void *context, unsigned slot, void *task, struct ss_control_transfer *transfer)
{ (void)context; assert(slot == 0 && task == &task_storage && home == current_cpu); assert(home == transfer->old_owner); return true; }
static void adopt(void *context, unsigned slot, void *task, const struct ss_control_transfer *transfer)
{ (void)context; assert(slot == 0 && task == &task_storage && current_cpu == transfer->new_owner); home = transfer->new_owner; ++adoptions; }
static bool drained(void *context, unsigned cpu, unsigned slot)
{
    (void)context; assert(cpu == current_cpu && slot == 0);
    for (unsigned p = 0; p < SS_OWNER_PRIORITIES; ++p)
        if ((atomic_load_explicit(&run[cpu].incoming[p][0], memory_order_acquire) |
             run[cpu].detached[p][0]) & 1) return false;
    return true;
}
static void retired(void *context, unsigned slot, uint64_t incarnation)
{ (void)context; assert(slot == 0 && incarnation == 1); ++retirements; }
static void retirement(void *context, unsigned slot, unsigned coordinator)
{ (void)context; (void)coordinator; ss_control_retirement(&control, slot); }
static void inspect(void *context, unsigned slot, unsigned priority)
{
    (void)context; assert(slot == 0 && priority == 5);
    ++inspected[current_cpu];
    if (current_cpu != home) ss_control_forward(&control, slot);
}
static void pump(void)
{
    for (unsigned pass = 0; pass < 12; ++pass)
        for (current_cpu = 0; current_cpu < 3; ++current_cpu) {
            ss_control_drain(&control, current_cpu);
            ss_owner_drain(&run[current_cpu], inspect, NULL);
            if (ss_owner_commit_dispatch(&run[current_cpu], 6, false))
                ss_owner_continue(&run[current_cpu]);
        }
}
static void setup(void)
{
    const struct ss_control_ops ops = {
        .apply=apply, .detach=detach, .adopt=adopt, .drained=drained, .retired=retired
    };
    home = current_cpu = adoptions = retirements = 0;
    for (unsigned c = 0; c < 3; ++c) {
        atomic_store(&interrupts[c], 0); inspected[c] = 0;
        ss_owner_init(&run[c], c, &backend);
        assert(!ss_owner_commit_dispatch(&run[c], 6, false));
    }
    ss_control_init(&control, 7, &backend, &ops);
    struct ss_owner_retirement notification = { .notify=retirement, .slot=0 };
    ss_owner_descriptor_init_closed(&descriptor, 0, &notification);
    ss_control_register(&control, 0, &descriptor, 1, 0, 5, 1);
    assert(ss_owner_reopen(&descriptor, &task_storage, 1, true));
}
static bool affinity(void *context, struct ss_control_state *state)
{ state->affinity = (uint64_t)(uintptr_t)context; return true; }
static bool deletion(void *context, struct ss_control_state *state)
{ (void)context; state->dead = true; ss_owner_close(&descriptor, &backend); return true; }
static uint64_t command(ss_control_mutate mutate, uintptr_t argument)
{
    assert(ss_owner_pin(&descriptor, 1, &backend));
    uint64_t sequence = ss_control_publish(&control, 0, mutate, (void *)argument);
    ss_owner_put(&descriptor);
    return sequence;
}
static void delayed_publisher(void)
{
    setup();
    assert(ss_owner_pin(&descriptor, 1, &backend));
    struct ss_control_route old, next;
    assert(ss_control_route_pin(&control, 0, &old));
    assert(old.owner == 0 && old.epoch == 0);
    command(affinity, 2); pump();
    assert(home == 1 && control.slot[0].phase == 3 && !control.slot[0].old_drained);
    assert(ss_control_route_pin(&control, 0, &next));
    assert(next.owner == 1 && next.epoch == 1);
    ss_owner_publish(&run[old.owner], 0, 5);
    ss_control_route_put(&old);
    current_cpu = 0;
    ss_control_drain(&control, 0);
    assert(!control.slot[0].old_drained); /* the old hint is not drained yet */
    pump();
    assert(inspected[0] && control.slot[0].phase == 0);
    assert(next.control && !ss_owner_references_retired(&descriptor));
    ss_control_route_put(&next); ss_owner_put(&descriptor);
}
static void *stale_snapshot(void *unused)
{
    (void)unused;
    assert(ss_owner_pin(&descriptor, 1, &backend));
    pause_sample = true;
    struct ss_control_route route;
    bool admitted = ss_control_route_pin(&control, 0, &route);
    if (admitted) {
        fputs("stale route admitted after bank reuse\n", stderr);
        abort();
    }
    /* Exactly one failed attempt: epoch-neutral activation reaches every owner. */
    for (unsigned c = 0; c < 3; ++c) ss_owner_publish(&run[c], 0, 5);
    ss_owner_put(&descriptor);
    return NULL;
}
static void two_transfers(void)
{
    setup(); atomic_store(&sampled, 0); atomic_store(&proceed, 0);
    pthread_t thread; assert(!pthread_create(&thread, NULL, stale_snapshot, NULL));
    while (!atomic_load_explicit(&sampled, memory_order_acquire)) sched_yield();
    command(affinity, 2); pump(); assert(home == 1 && control.slot[0].phase == 0);
    command(affinity, 4); pump(); assert(home == 2 && control.slot[0].phase == 0);
    atomic_store_explicit(&proceed, 1, memory_order_release);
    assert(!pthread_join(thread, NULL)); pump();
    assert(adoptions == 2 && inspected[2] == 1);
    assert(atomic_load(&control.slot[0].route_readers[0]) == 0);
}
static void delete_during_transfer(void)
{
    setup();
    assert(ss_owner_pin(&descriptor, 1, &backend));
    struct ss_control_route old;
    assert(ss_control_route_pin(&control, 0, &old));
    command(affinity, 2);
    current_cpu = 0; ss_control_drain(&control, 0);
    assert(control.slot[0].phase == 2 && home == 0);
    command(deletion, 0); pump();
    assert(home == 1 && adoptions == 1 && !retirements);
    ss_owner_publish(&run[old.owner], 0, 5);
    ss_control_route_put(&old); ss_owner_put(&descriptor); pump();
    assert(retirements && ss_owner_references_retired(&descriptor));
}
int main(void)
{
    delayed_publisher(); two_transfers(); delete_during_transfer();
    puts("scheduler routes: late publication, fresh drain, two-bank stale snapshot and transfer deletion pass");
}
