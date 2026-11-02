/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../kernel/scheduler/freertos_control.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
static struct ss_control control;
static struct ss_owner_descriptor descriptor;
static _Atomic bool entered, release_consumer, registered;
static _Atomic uint64_t retired_incarnation;
static bool apply(void *context, unsigned slot, void *task, const struct ss_control_state *state)
{ (void)context; (void)slot; (void)task; (void)state; return true; }
static bool drained(void *context, unsigned cpu, unsigned slot)
{
    (void)context; (void)cpu; (void)slot;
    atomic_store(&entered, true);
    while (!atomic_load(&release_consumer)) sched_yield();
    return true;
}
static void retired(void *context, unsigned slot, uint64_t incarnation)
{ (void)context; (void)slot; atomic_store(&retired_incarnation, incarnation); }
static void *consume(void *context)
{ (void)context; assert(ss_control_drain(&control, 0) == 1); return NULL; }
static void *reuse(void *context)
{
    (void)context;
    ss_control_register(&control, 0, &descriptor, 2, 0, 3, 1);
    atomic_store(&registered, true);
    return NULL;
}
int main(void)
{
    struct ss_control_ops ops = { .apply = apply, .drained = drained, .retired = retired };
    ss_owner_descriptor_init_closed(&descriptor, 1, NULL);
    ss_control_init(&control, 1, NULL, &ops);
    ss_control_register(&control, 0, &descriptor, 1, 0, 1, 1);
    /* A duplicate retirement hint may outlive the last reference and the
     * registry's first successful retirement. The consumer owns no TCB pin. */
    control.slot[0].desired.dead = true;
    atomic_store(&control.pending[0][0], 1);
    pthread_t consumer, registrar;
    assert(!pthread_create(&consumer, NULL, consume, NULL));
    while (!atomic_load(&entered)) sched_yield();
    uint32_t serving = atomic_load(&control.slot[0].serving);
    assert(!pthread_create(&registrar, NULL, reuse, NULL));
    while (atomic_load(&control.slot[0].tickets) != serving + 2 && !atomic_load(&registered))
        sched_yield();
    assert(!atomic_load(&registered));
    assert(atomic_load(&control.slot[0].serving) == serving);
    atomic_store(&release_consumer, true);
    assert(!pthread_join(consumer, NULL));
    assert(!pthread_join(registrar, NULL));
    assert(atomic_load(&retired_incarnation) == 1);
    assert(control.slot[0].desired.incarnation == 2);
    assert(!control.slot[0].desired.dead && !control.slot[0].retained);
    assert(atomic_load(&control.slot[0].tickets) == atomic_load(&control.slot[0].serving));
    puts("scheduler control reuse: queued closed-slot consumer preserves permanent FIFO; stale retirement retains incarnation");
    return 0;
}
