/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../kernel/scheduler/owner.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static unsigned inbox_loads, entry_loads, spill_exchanges;
void ss_owner_test_scan(unsigned kind)
{ if (kind == 0) ++inbox_loads; else if (kind == 1) ++entry_loads; else ++spill_exchanges; }
static void reset_scan(void) { inbox_loads = entry_loads = spill_exchanges = 0; }
static unsigned trace_slots[16], trace_priorities[16], trace_count;
static bool inject_urgent;
static void record(void *ctx, unsigned slot, unsigned priority)
{
    struct ss_owner *o = ctx;
    assert(trace_count < 16);
    trace_slots[trace_count] = slot;
    trace_priorities[trace_count++] = priority;
    if (inject_urgent && trace_count == 1) ss_owner_publish(o, 1, 127, 7);
}
/* An empty merge reads the inbox line only: eight counts and the spill count. */
static void scan_costs(void)
{
    struct ss_owner o;
    ss_owner_init(&o, 0, NULL);
    reset_scan();
    assert(ss_owner_drain(&o, record, &o) == 0);
    assert(inbox_loads == 9 && entry_loads == 0 && spill_exchanges == 0);
    for (unsigned i = 0; i < 8; ++i) {
        ss_owner_publish(&o, 2, i, 7);
        ss_owner_publish(&o, 2, 64 + i, 7);
    }
    reset_scan(); trace_count = 0;
    assert(ss_owner_drain(&o, record, &o) == 16);
    /* Sixteen merges; only the first reads the sixteen entries. */
    assert(inbox_loads == 16 * 9 && entry_loads == 16 && spill_exchanges == 0);
    assert(!ss_owner_has_pending(&o));
    ss_owner_publish(&o, 0, 0, 3); ss_owner_publish(&o, 0, 1, 3);
    reset_scan(); trace_count = 0;
    inject_urgent = true;
    assert(ss_owner_drain(&o, record, &o) == 3);
    inject_urgent = false;
    assert(trace_slots[0] == 0 && trace_priorities[0] == 3);
    assert(trace_slots[1] == 127 && trace_priorities[1] == 7);
    assert(trace_slots[2] == 1 && trace_priorities[2] == 3);
    assert(entry_loads == 3 && spill_exchanges == 0);
}
/* A source that runs SS_OWNER_RING hints ahead of the owner spills into the
 * bitmap, reads consumed only when its copy says the outbox is full, and
 * returns to its outbox once the owner has taken the entries. */
static unsigned seen_slots[128];
static void count_any(void *ctx, unsigned slot, unsigned priority)
{ (void)ctx; (void)priority; assert(slot < 128); ++seen_slots[slot]; }
static void ring_spill(void)
{
    struct ss_owner o;
    ss_owner_init(&o, 0, NULL);
    for (unsigned i = 0; i < 40; ++i) ss_owner_publish(&o, 3, i, 4);
    assert(atomic_load(&o.inbox.published[3]) == SS_OWNER_RING);
    assert(atomic_load(&o.inbox.spilled) == 40 - SS_OWNER_RING);
    for (unsigned i = 0; i < 40; ++i) assert(ss_owner_slot_pending(&o, i));
    assert(!ss_owner_slot_pending(&o, 40));
    reset_scan();
    for (unsigned i = 0; i < 3; ++i) (void)ss_owner_drain(&o, count_any, NULL);
    assert(!ss_owner_has_pending(&o));
    assert(spill_exchanges == 1);
    for (unsigned i = 0; i < 128; ++i) assert(seen_slots[i] == (i < 40));
    assert(atomic_load(&o.consumed[3]) == SS_OWNER_RING);
    /* The next publication refreshes the limit and uses the outbox again. */
    ss_owner_publish(&o, 3, 50, 4);
    assert(atomic_load(&o.inbox.limit[3]) == SS_OWNER_RING);
    assert(atomic_load(&o.inbox.published[3]) == SS_OWNER_RING + 1);
    assert(atomic_load(&o.inbox.spilled) == 40 - SS_OWNER_RING);
    assert(ss_owner_slot_pending(&o, 50) && ss_owner_has_pending(&o));
    assert(ss_owner_drain(&o, count_any, NULL) == 1 && seen_slots[50] == 1);
    /* Counts wrap at 16 bits without losing a hint. */
    for (unsigned round = 0; round < 3000; ++round) {
        for (unsigned i = 0; i < 30; ++i) ss_owner_publish(&o, 3, 60 + i, 1);
        while (ss_owner_drain(&o, count_any, NULL)) {}
    }
    assert(atomic_load(&o.inbox.spilled) == 40 - SS_OWNER_RING);
    for (unsigned i = 60; i < 90; ++i) assert(seen_slots[i] == 3000);
    for (unsigned i = 0; i < 128; ++i) seen_slots[i] = 0;
}
static unsigned retirement_events;
static void retirement_event(void *context, unsigned slot, unsigned cpu)
{
    assert(context == &retirement_events && slot == 77 && cpu == 0);
    ++retirement_events;
}
static void close_in_pin_barrier(void *context)
{ ss_owner_close(context, NULL); }
static void retirement_progress(void)
{
    struct ss_owner_descriptor d;
    const struct ss_owner_retirement retirement = {
        .notify = retirement_event, .context = &retirement_events,
        .slot = 77, .retirement_owner = 0
    };
    ss_owner_descriptor_init_closed(&d, 1, &retirement);
    assert(!retirement_events);
    assert(ss_owner_reopen(&d, &d, 2, true));
    assert(ss_owner_pin(&d, 2, NULL));
    ss_owner_put(&d); /* Ordinary open last release must not notify. */
    assert(!retirement_events);
    ss_owner_close(&d, NULL); /* No publisher remains to generate an event. */
    assert(retirement_events == 1);
    assert(ss_owner_reopen(&d, &d, 3, true));
    assert(ss_owner_pin(&d, 3, NULL));
    assert(ss_owner_pin(&d, 3, NULL));
    ss_owner_close(&d, NULL);
    assert(retirement_events == 1);
    ss_owner_put(&d); assert(retirement_events == 1);
    ss_owner_put(&d); assert(retirement_events == 2);
    assert(ss_owner_reopen(&d, &d, 4, true));
    /* Pause a stale-key pin after increment. Close sees its transient reference,
     * so rejection's internal put is the only remaining retirement wake. */
    const struct ss_owner_backend close_backend = {
        .barrier = close_in_pin_barrier, .context = &d
    };
    assert(!ss_owner_pin(&d, 3, &close_backend));
    assert(retirement_events == 3 && ss_owner_references_retired(&d));
    assert(!ss_owner_pin(&d, 4, NULL)); /* Closed pin also releases internally. */
    assert(retirement_events == 4 && ss_owner_references_retired(&d));
    assert(ss_owner_reopen(&d, &d, 5, true));
    assert(!ss_owner_pin(&d, 4, NULL)); /* Open incarnation mismatch is silent. */
    assert(retirement_events == 4);
}
static _Atomic unsigned sgis;
static void sgi(void *ctx, unsigned cpu) { (void)ctx; assert(cpu == 3); ++sgis; }
static const struct ss_owner_backend backend = { .sgi = sgi };
/* Concurrent sources against a draining owner. Each source publishes a round
 * of its own slots, more than one outbox holds, writing the round into want[]
 * before each publication; the owner copies want[] into got[] when it
 * inspects a slot. A source starts its next round only after the owner has
 * seen this one, so a hint lost to a lapped entry, a missed spill or a stale
 * count stops that source, and the deadline fails the check. */
#define RACE_SOURCES 3u
#define RACE_SLOTS 40u
#define RACE_ROUNDS 3000u
static _Atomic unsigned want[128], got[128];
static _Atomic bool race_done;
static struct ss_owner race;
static void race_inspect(void *ctx, unsigned slot, unsigned priority)
{
    (void)ctx; (void)priority;
    assert(slot < RACE_SOURCES * RACE_SLOTS);
    atomic_store(&got[slot], atomic_load(&want[slot]));
}
static void *race_source(void *argument)
{
    unsigned source = (unsigned)(uintptr_t)argument;
    for (unsigned round = 1; round <= RACE_ROUNDS; ++round) {
        for (unsigned i = 0; i < RACE_SLOTS; ++i) {
            unsigned slot = source * RACE_SLOTS + i;
            atomic_store(&want[slot], round);
            ss_owner_publish(&race, source, slot, (slot + round) % 8);
        }
        struct timespec start, now;
        clock_gettime(CLOCK_MONOTONIC, &start);
        for (unsigned i = 0; i < RACE_SLOTS; ++i)
            while (atomic_load(&got[source * RACE_SLOTS + i]) < round) {
                clock_gettime(CLOCK_MONOTONIC, &now);
                if (now.tv_sec - start.tv_sec > 10) {
                    fprintf(stderr, "source %u round %u: slot %u never inspected\n",
                            source, round, source * RACE_SLOTS + i);
                    abort();
                }
                sched_yield();
            }
    }
    return NULL;
}
static void *race_owner(void *unused)
{
    (void)unused;
    while (!atomic_load(&race_done))
        if (!ss_owner_drain(&race, race_inspect, NULL)) sched_yield();
    return NULL;
}
static void concurrent_sources(void)
{
    ss_owner_init(&race, 3, &backend);
    unsigned before = sgis;
    pthread_t sources[RACE_SOURCES], consumer;
    assert(!pthread_create(&consumer, NULL, race_owner, NULL));
    for (unsigned s = 0; s < RACE_SOURCES; ++s)
        assert(!pthread_create(&sources[s], NULL, race_source, (void *)(uintptr_t)s));
    for (unsigned s = 0; s < RACE_SOURCES; ++s) assert(!pthread_join(sources[s], NULL));
    atomic_store(&race_done, true);
    assert(!pthread_join(consumer, NULL));
    assert(!ss_owner_has_pending(&race));
    /* Every publication signals. */
    assert(sgis - before == RACE_SOURCES * RACE_SLOTS * RACE_ROUNDS);
    printf("scheduler owner: %u concurrent hints, %u spilled, none lost\n",
           RACE_SOURCES * RACE_SLOTS * RACE_ROUNDS, (unsigned)atomic_load(&race.inbox.spilled));
    sgis = 0;
}
static struct ss_owner owner;
static unsigned visited[128], visits;
static void inspect(void *ctx, unsigned slot, unsigned priority)
{
    struct ss_owner *o = ctx;
    assert(priority == 5); ++visited[slot]; ++visits;
    /* Replenishing the first word must not starve the detached second word. */
    ss_owner_publish(o, 0, 0, 5);
}
static struct ss_owner_descriptor descriptor;
static _Atomic unsigned phase;
static void *publisher(void *unused)
{
    (void)unused;
    assert(ss_owner_pin(&descriptor, 11, NULL));
    atomic_store(&phase, 1);
    while (atomic_load(&phase) != 2) sched_yield();
    assert(descriptor.task == &owner);
    ss_owner_publish(&owner, 1, 127, 7);
    ss_owner_put(&descriptor);
    return NULL;
}
int main(void)
{
    scan_costs();
    ring_spill();
    concurrent_sources();
    retirement_progress();
    ss_owner_init(&owner, 3, &backend);
    struct ss_owner_task a = {.slot=1,.priority=2}, b = {.slot=2,.priority=2};
    struct ss_owner_task c = {.slot=3,.priority=7};
    assert(ss_owner_enqueue(&owner,&a,false));
    assert(ss_owner_enqueue(&owner,&b,false));
    assert(!ss_owner_enqueue(&owner,&a,false));
    assert(ss_owner_select(&owner)==&a && !a.queued);
    assert(ss_owner_yield(&owner)==&b);
    assert(ss_owner_enqueue(&owner,&c,false));
    ss_owner_preempt(&owner);
    assert(ss_owner_select(&owner)==&c);
    owner.current=NULL;
    assert(ss_owner_select(&owner)==&b);
    assert(ss_owner_remove(&owner,&a));
    owner.current=NULL;
    assert(!ss_owner_select(&owner) && !owner.ready_bitmap);
    assert(!ss_owner_has_pending(&owner));
    ss_owner_publish(&owner, 0, 9, 5); assert(sgis==1);
    assert(ss_owner_has_pending(&owner) && ss_owner_slot_pending(&owner, 9));
    ss_owner_publish(&owner, 1, 10, 5); assert(sgis==2);
    ss_owner_publish(&owner, 0, 11, 5); assert(sgis==3);
    ss_owner_publish(&owner, 2, 127, 5); assert(sgis==4);
    ss_owner_continue(&owner); assert(sgis==5);
    for (unsigned i=0; i<8 && !visited[127]; ++i)
        assert(ss_owner_drain(&owner,inspect,&owner)<=16);
    assert(visited[127] && visits<=128);
    ss_owner_descriptor_init(&descriptor, &owner, 11);
    pthread_t thread; assert(!pthread_create(&thread,NULL,publisher,NULL));
    while (atomic_load(&phase)!=1) sched_yield();
    ss_owner_close(&descriptor,NULL);
    assert(!ss_owner_pin(&descriptor,11,NULL));
    assert(!ss_owner_references_retired(&descriptor));
    assert(!ss_owner_reopen(&descriptor,&descriptor,12,true));
    atomic_store(&phase,2); assert(!pthread_join(thread,NULL));
    assert(ss_owner_references_retired(&descriptor));
    assert(!ss_owner_reopen(&descriptor,&descriptor,12,false));
    /* Real integration must drain every owner before granting this proof. */
    assert(ss_owner_reopen(&descriptor,&descriptor,12,true));
    assert(!ss_owner_pin(&descriptor,11,NULL));
    assert(ss_owner_pin(&descriptor,12,NULL)); ss_owner_put(&descriptor);
    puts("scheduler owner: FIFO, bounded fairness, mailbox and retirement pass");
}
