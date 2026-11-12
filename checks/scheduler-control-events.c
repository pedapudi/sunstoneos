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
    ++waits;
    assert(!"a control drain must not wait behind an interrupted publisher");
}
static bool apply(void *context, unsigned slot, void *task, const struct ss_control_state *state)
{ (void)context; (void)task; (void)state; return slot != 0; }
static void retired(void *context, unsigned slot, uint64_t incarnation)
{ (void)context; assert(slot == 2 && incarnation == 1); completion = true; }
static bool change(void *context, struct ss_control_state *state)
{ (void)context; ++state->resume_generation; return true; }
static bool change_while_owner_drains(void *context, struct ss_control_state *state)
{
    (void)context;
    assert(ss_control_drain(&control, 0) == 2 && waits == 0);
    assert(visible_serving == atomic_load_explicit(&control.slot[0].serving, memory_order_acquire) &&
           "previous FIFO release must precede the next wait");
    assert(atomic_load(&control.parked[0].word[0]) & 2);
    ++state->resume_generation;
    return true;
}
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
    visible_serving = atomic_load_explicit(&control.slot[0].serving, memory_order_relaxed);
    /* The owner tries slot 1 while this real publisher still owns its FIFO. */
    ss_control_publish(&control, 1, change_while_owner_drains, NULL);
    assert(!(atomic_load(&control.parked[0].word[0]) & 2));
    assert(ss_control_drain_slot(&control, 0, 1) == 1);

    ss_control_publish(&control, 2, close_task, &descriptor[2]);
    assert(ss_control_drain_slot(&control, 0, 2) == 1);
    assert(!completion && !ss_owner_references_retired(&descriptor[0]));
    ss_control_retirement(&control, 2);
    assert(ss_control_drain_slot(&control, 0, 2) == 1);
    assert(completion && visible_completion && "retirement callback must precede its completion event");
    puts("scheduler control events: FIFO handoff and retirement completion wake observers");
    return 0;
}
