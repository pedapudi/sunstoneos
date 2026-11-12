/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../kernel/scheduler/freertos_control.h"
#include <assert.h>
#include <stdio.h>

/* Model the event observer, rather than treating an atomic store as a wake.
 * A drain that waits on its next FIFO must first wake the previous FIFO's
 * waiters. Retirement completion needs an event after its callback as well.
 * This checks software ordering; it does not model board timing. */
static struct ss_control control;
static struct ss_owner_descriptor descriptor[3];
static unsigned visible_serving, waits;
static bool completion, visible_completion;

static void event(void *context)
{
    (void)context;
    visible_serving = atomic_load_explicit(&control.slot[0].serving, memory_order_acquire);
    visible_completion = completion;
}
static void wait(void *context)
{
    (void)context;
    assert(visible_serving == atomic_load_explicit(&control.slot[0].serving, memory_order_acquire) &&
           "previous FIFO release must precede the next wait");
    /* The other owner can now release the ticket that this drain awaits. */
    assert(!waits++);
    atomic_fetch_add_explicit(&control.slot[1].serving, 1, memory_order_release);
}
static bool apply(void *context, unsigned slot, void *task, const struct ss_control_state *state)
{ (void)context; (void)task; (void)state; return slot != 0; }
static void retired(void *context, unsigned slot, uint64_t incarnation)
{ (void)context; assert(slot == 2 && incarnation == 1); completion = true; }
static bool change(void *context, struct ss_control_state *state)
{ (void)context; ++state->resume_generation; return true; }
static bool close_task(void *context, struct ss_control_state *state)
{ ss_owner_close(context, NULL); state->dead = true; return true; }

int main(void)
{
    struct ss_owner_backend backend = { .event = event, .wait = wait };
    struct ss_control_ops ops = { .apply = apply, .retired = retired };
    ss_control_init(&control, 1, &backend, &ops);
    for (unsigned slot = 0; slot < 3; ++slot) {
        ss_owner_descriptor_init(&descriptor[slot], &descriptor[slot], 1);
        ss_control_register(&control, slot, &descriptor[slot], 1, 0, 1, 1);
    }
    ss_control_publish(&control, 0, change, NULL);
    ss_control_publish(&control, 1, change, NULL);
    /* Reserve one ticket for a remote holder before the drain visits slot 1. */
    atomic_fetch_add_explicit(&control.slot[1].tickets, 1, memory_order_relaxed);
    visible_serving = atomic_load_explicit(&control.slot[0].serving, memory_order_relaxed);
    assert(ss_control_drain(&control, 0) == 2 && waits == 1);

    ss_control_publish(&control, 2, close_task, &descriptor[2]);
    assert(ss_control_drain_slot(&control, 0, 2) == 1);
    assert(!completion && !ss_owner_references_retired(&descriptor[0]));
    ss_control_retirement(&control, 2);
    assert(ss_control_drain_slot(&control, 0, 2) == 1);
    assert(completion && visible_completion && "retirement callback must precede its completion event");
    puts("scheduler control events: FIFO handoff and retirement completion wake observers");
    return 0;
}
