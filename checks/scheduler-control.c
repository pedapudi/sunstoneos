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
static void drain(void)
{
    for (unsigned n = 0; n < 32; ++n)
        for (unsigned cpu = 0; cpu < 2; ++cpu) ss_control_drain(&control, cpu);
}
int main(void)
{
    struct ss_owner_backend backend = { .wait = wait_event };
    struct ss_control_ops ops = { .apply = apply, .detach = detach, .adopt = adopt,
        .drained = drained, .retired = retirement, .dispatch = dispatch };
    ss_control_init(&control, 3, &backend, &ops);
    for (unsigned slot = 0; slot < SS_OWNER_SLOTS; ++slot) {
        ss_owner_descriptor_init(&descriptor[slot], &descriptor[slot], 1);
        ss_control_register(&control, slot, &descriptor[slot], 1, 0, 1, 3);
    }
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
    seq = ss_control_publish(&control, 0, migrate, NULL);
    drain(); assert(!ss_control_applied(&control, 0, seq) && !atomic_load(&adopted));
    atomic_store(&offcpu, true); drain();
    assert(atomic_load(&adopted) == 1 && control.slot[0].phase == 3);
    ss_control_forward(&control, 0); drain();
    ss_owner_put(&descriptor[0]); drain(); assert(control.slot[0].phase == 0);
    assert(ss_owner_pin(&descriptor[0], 1, NULL));
    ss_control_publish(&control, 0, delete_task, &descriptor[0]); drain();
    assert(!atomic_load(&retired));
    ss_owner_put(&descriptor[0]); ss_control_retirement(&control, 0); drain();
    assert(atomic_load(&retired));
    for (unsigned n = 0; n < 2; ++n) assert(!pthread_create(&threads[n], NULL, gate_caller, (void *)(uintptr_t)n));
    for (unsigned n = 0; n < 2; ++n) assert(!pthread_join(threads[n], NULL));
    assert(atomic_load(&control.gate_phase) == SS_GATE_OPEN);
    puts("scheduler control: concurrent publication, off-CPU suspend/transfer, late publisher, retirement and FIFO nested gate passed");
    return 0;
}
