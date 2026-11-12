/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../kernel/scheduler/freertos_control.h"
#include "../kernel/scheduler/notification.h"
#include <assert.h>
#include <stdio.h>
#ifdef SUNSTONE_SCHED_LATENCY_TRACE
#include "../kernel/scheduler/latency_trace.h"
static unsigned busy_records;
uint64_t ss_sched_trace_test_now(void) { static uint64_t now; return ++now; }
void ss_sched_trace_record(unsigned cpu, const struct ss_sched_trace_record *record)
{
    assert(cpu == 0);
    if (record->phase != SS_SCHED_RECORD_FIFO_BUSY) return;
    assert(record->stamp[0] < record->stamp[1] && record->stamp[1] < record->stamp[2]);
    assert(record->stamp[3] == record->stamp[2] && record->stamp[4] == record->stamp[2]);
    ++busy_records;
}
#endif

static struct ss_control control;
static struct ss_owner_descriptor descriptor[4];
static unsigned calls[4], signals, injection, park_injection;
static unsigned race_resource;
static bool offcpu, repaired, injecting;

static bool suspend(void *context, struct ss_control_state *state)
{ (void)context; state->suspended = true; return true; }
#ifdef SS_CONTROL_OLD_PUBLICATION_NEGATIVE
static bool resume(void *context, struct ss_control_state *state)
{ (void)context; state->suspended = false; ++state->resume_generation; return true; }
#endif
static bool change(void *context, struct ss_control_state *state)
{ (void)context; ++state->resume_generation; return true; }
static void wait(void *context)
{ (void)context; assert(!"interrupted publisher cannot release this FIFO"); }
static void signal(void *context, unsigned cpu)
{ (void)context; assert(cpu == 0); ++signals; }
static void barrier(void *context)
{
    (void)context;
    if (injection != 1 || injecting) return;
    injecting = true;
#ifdef SS_CONTROL_OLD_PUBLICATION_NEGATIVE
    ss_control_publish(&control, 0, resume, NULL);
#else
    assert(ss_control_resume_isr(&control, 0));
#endif
    injecting = false;
    injection = 0;
}
static bool apply(void *context, unsigned slot, void *task, const struct ss_control_state *state)
{
    (void)context; (void)task;
    ++calls[slot];
    if (slot == 1 && !repaired) return false;
    if (state->offcpu_sequence && !offcpu) {
        ss_control_park_on(&control, 0, slot, &offcpu);
        return false;
    }
    return true;
}
static bool deferred(void *context, unsigned slot)
{ (void)context; return (atomic_load(&control.parked[0].word[slot / 64]) >> (slot % 64)) & 1; }
static void initialize(void)
{
    static const struct ss_owner_backend backend = { .barrier = barrier, .wait = wait, .sgi = signal };
    struct ss_control_ops ops = { .apply = apply, .deferred = deferred };
    ss_control_init(&control, 1, &backend, &ops);
    for (unsigned i = 0; i < 4; ++i) {
        ss_owner_descriptor_init(&descriptor[i], &descriptor[i], 1);
        ss_control_register(&control, i, &descriptor[i], 1, 0, 1, 1);
        calls[i] = 0;
    }
    signals = injection = park_injection = 0; injecting = false; offcpu = repaired = false;
}
void ss_control_test_park(struct ss_control *c, unsigned cpu, unsigned slot, unsigned stage)
{
    assert(c == &control && cpu == 0);
    unsigned wanted = park_injection <= 2 ? park_injection - 1u : 2u;
    if (!park_injection || stage != wanted) return;
    unsigned kind = park_injection;
    park_injection = 0;
    if (kind >= 3) {
        if (kind == 3) ss_control_wake_on(c, cpu, slot, &race_resource);
        else {
            uint64_t records[2] = { UINT64_C(1) << slot, 0 };
            ss_control_wake_mask(c, cpu, records, &race_resource);
        }
        return;
    }
    atomic_store(&control.slot[slot].serving, atomic_load(&control.slot[slot].tickets));
    if (kind == 2) {
        struct ss_control_state snapshot;
        ss_control_snapshot(c, slot, &snapshot);
    }
}
static unsigned drain(uint64_t seen[2])
{
#ifdef SS_CONTROL_REVISIT_NEGATIVE
    (void)seen; return ss_control_drain(&control, 0);
#else
    return ss_control_drain_excluding(&control, 0, seen);
#endif
}
static void self_resume_and_fence(void)
{
    initialize(); injection = 1;
#ifdef SS_CONTROL_OLD_PUBLICATION_NEGATIVE
    ss_control_publish(&control, 0, suspend, NULL);
#else
    assert(ss_control_suspend_current(&control, 0, 0));
#endif
    assert(!injection && ss_control_suspension(&control, 0) == (SS_CONTROL_OFFCPU | SS_CONTROL_SELF_FRAME));
    uint64_t seen[2] = {0};
    assert(drain(seen) == 1 && calls[0] == 1);
    assert(!ss_control_has_pending(&control, 0));
    for (unsigned n = 0; n < 8; ++n) {
        uint64_t later[2] = {0};
        assert(drain(later) == 0 && calls[0] == 1);
    }
    offcpu = true;
    ss_control_wake_on(&control, 0, 0, &offcpu);
    uint64_t next[2] = {0};
    assert(drain(next) == 1 && calls[0] == 2);
    assert(!ss_control_suspension(&control, 0) && !control.slot[0].desired.suspended);
    assert(control.slot[0].desired.resume_generation == 1);
    assert(!atomic_load(&descriptor[0].references));
}
static void one_visit_per_epoch(void)
{
    initialize();
    ss_control_publish(&control, 1, change, NULL);
    uint64_t seen[2] = {0};
    assert(drain(seen) == 1 && calls[1] == 1);
    repaired = true;
    assert(drain(seen) == 0 && calls[1] == 1 && "same record must wait for a new maintenance epoch");
    uint64_t next[2] = {0};
    assert(drain(next) == 1 && calls[1] == 2);
}
static void suspension_sources(void)
{
    initialize();
    assert(ss_control_suspend_current(&control, 0, 0));
    assert(ss_control_resume_isr(&control, 0));
    ss_control_publish(&control, 0, suspend, NULL);
    unsigned flags = ss_control_suspension(&control, 0);
    assert((flags & (SS_CONTROL_SUSPENDED | SS_CONTROL_SELF | SS_CONTROL_SELF_FRAME)) ==
           (SS_CONTROL_SUSPENDED | SS_CONTROL_SELF_FRAME));
    offcpu = true;
    uint64_t first[2] = {0};
    assert(drain(first) == 1 && !atomic_load(&descriptor[0].references));
    initialize();
    ss_control_publish(&control, 0, suspend, NULL);
    assert(ss_control_resume_isr(&control, 0));
    assert(ss_control_suspend_current(&control, 0, 0));
    offcpu = true;
    uint64_t seen[2] = {0};
    assert(drain(seen) == 1);
    assert(ss_control_suspension(&control, 0) == (SS_CONTROL_SUSPENDED | SS_CONTROL_SELF));
    assert(ss_control_resume_isr(&control, 0));
    ss_control_publish(&control, 0, suspend, NULL);
    flags = ss_control_suspension(&control, 0);
    assert((flags & (SS_CONTROL_SELF | SS_CONTROL_SELF_FRAME)) == 0);
    uint64_t later[2] = {0};
    assert(drain(later) == 1 && !atomic_load(&descriptor[0].references));
}
static bool drain_while_locked(void *context, struct ss_control_state *state)
{
    (void)context;
    uint64_t seen[2] = {0};
    unsigned before = signals;
    assert(drain(seen) == 1 && !calls[2]);
#ifdef SUNSTONE_SCHED_LATENCY_TRACE
    assert(busy_records);
#endif
    assert(!ss_control_has_pending(&control, 0));
    for (unsigned n = 0; n < 8; ++n) {
        uint64_t later[2] = {0};
        assert(drain(later) == 0 && signals == before);
    }
    ++state->resume_generation;
    return true;
}
static void fifo_release_and_resource_cookie(void)
{
    initialize();
    ss_control_publish(&control, 2, change, NULL);
    ss_control_publish(&control, 2, drain_while_locked, NULL);
    assert(ss_control_has_pending(&control, 0));
    uint64_t seen[2] = {0};
    assert(drain(seen) == 1 && calls[2] == 1);
    int first = 0, second = 0;
    ss_control_park_on(&control, 0, 2, &first);
    ss_control_wake_on(&control, 0, 2, &first);
    ss_control_park_on(&control, 0, 2, &second);
    unsigned before = signals;
    ss_control_wake_on(&control, 0, 2, &first);
    assert(!ss_control_has_pending(&control, 0) && signals == before);
    ss_control_wake_on(&control, 0, 2, &second);
    assert(ss_control_has_pending(&control, 0) && signals == before + 1);
}
static void notification_wake(void *context, struct ss_notification *notification, uint64_t cpus)
{
    (void)context;
    assert(cpus == 1);
    ss_control_wake_on(&control, 0, 3, notification);
}
static void notification_release(void)
{
    initialize();
    struct ss_notification notification;
    ss_notification_init(&notification, 0);
    notification.owner_wake = notification_wake;
    struct ss_notification_guard holder, attempt;
    ss_notification_gate_begin(&notification, control.backend, &holder);
    uint64_t ticket = atomic_load(&notification.next_ticket), word = atomic_load(&notification.word);
    assert(!ss_notification_gate_try_begin(&notification, control.backend, &attempt));
    assert(atomic_load(&notification.next_ticket) == ticket && atomic_load(&notification.word) == word);
    ss_control_publish(&control, 3, change, NULL);
    assert(atomic_load(&descriptor[3].references) == 1);
    ss_control_park_on(&control, 0, 3, &notification);
    ss_notification_owner_wait(&notification, 0, control.backend);
    assert(!ss_control_has_pending(&control, 0));
    ss_notification_gate_end(&holder);
    assert(ss_control_has_pending(&control, 0));
    assert(ss_notification_gate_try_begin(&notification, control.backend, &attempt));
    ss_notification_gate_end(&attempt);
    uint64_t seen[2] = {0};
    assert(drain(seen) == 1 && !atomic_load(&descriptor[3].references));
    /* Release precedes registration: the fresh empty check supplies the wake. */
    ss_control_park_on(&control, 0, 3, &notification);
    unsigned before = signals;
    ss_notification_owner_wait(&notification, 0, control.backend);
    assert(ss_control_has_pending(&control, 0) && signals == before + 1);
}
static void release_registration_races(void)
{
    for (unsigned kind = 1; kind <= 2; ++kind) {
        initialize();
        ss_control_publish(&control, 2, change, NULL);
        atomic_fetch_add(&control.slot[2].tickets, 1); /* a remote holder */
        park_injection = kind;
        unsigned before = signals;
        uint64_t seen[2] = {0};
        assert(drain(seen) == 1 && !park_injection);
        assert(ss_control_has_pending(&control, 0) && signals == before + 1);
        uint64_t next[2] = {0};
        assert(drain(next) == 1 && calls[2] == 1);
    }
}
static void release_during_repark(void)
{
    for (unsigned kind = 3; kind <= 4; ++kind) {
        initialize();
        ss_control_publish(&control, 2, change, NULL);
        ss_control_park_on(&control, 0, 2, &race_resource);
        park_injection = kind;
        ss_control_park_on(&control, 0, 2, &race_resource);
        assert(!park_injection);
        /* The released resource is free when the renewed subscription checks
         * it. This check must not leave the record permanently parked. */
        ss_control_wake_on(&control, 0, 2, &race_resource);
        assert(ss_control_has_pending(&control, 0) && "release during repark must not strand its parked bit");
        uint64_t seen[2] = {0};
        assert(drain(seen) == 1 && calls[2] == 1 && !atomic_load(&descriptor[2].references));
    }
}
int main(void)
{
    self_resume_and_fence();
    one_visit_per_epoch();
    suspension_sources();
    fifo_release_and_resource_cookie();
    notification_release();
    release_registration_races();
    release_during_repark();
    puts("PASS control atomic publication, frame fence, epoch bound and release-driven continuations");
    return 0;
}
