/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../kernel/scheduler/owner.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>

static unsigned scan_loads, scan_exchanges;
void ss_owner_test_scan(bool exchange)
{ if (exchange) ++scan_exchanges; else ++scan_loads; }
static unsigned trace_slots[16], trace_priorities[16], trace_count;
static bool inject_urgent;
static void record(void *ctx, unsigned slot, unsigned priority)
{
    struct ss_owner *o = ctx;
    assert(trace_count < 16);
    trace_slots[trace_count] = slot;
    trace_priorities[trace_count++] = priority;
    if (inject_urgent && trace_count == 1) ss_owner_publish(o, 127, 7);
}
static void scan_costs(void)
{
    struct ss_owner o;
    ss_owner_init(&o, 0, NULL);
    scan_loads = scan_exchanges = 0;
    assert(ss_owner_drain(&o, record, &o) == 0);
    assert(scan_loads == 16 && scan_exchanges == 0);
    for (unsigned i = 0; i < 8; ++i) {
        ss_owner_publish(&o, i, 7);
        ss_owner_publish(&o, 64 + i, 7);
    }
    scan_loads = scan_exchanges = trace_count = 0;
    assert(ss_owner_drain(&o, record, &o) == 16);
    assert(scan_loads == 16 && scan_exchanges == 2);
    assert(!ss_owner_has_pending(&o));
    ss_owner_publish(&o, 0, 3); ss_owner_publish(&o, 1, 3);
    scan_loads = scan_exchanges = trace_count = 0;
    inject_urgent = true;
    assert(ss_owner_drain(&o, record, &o) == 3);
    inject_urgent = false;
    assert(trace_slots[0] == 0 && trace_priorities[0] == 3);
    assert(trace_slots[1] == 127 && trace_priorities[1] == 7);
    assert(trace_slots[2] == 1 && trace_priorities[2] == 3);
    assert(scan_loads == 48 && scan_exchanges == 2);
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
static unsigned sgis;
static void sgi(void *ctx, unsigned cpu) { (void)ctx; assert(cpu == 3); ++sgis; }
static const struct ss_owner_backend backend = { .sgi = sgi };
static struct ss_owner owner;
static unsigned visited[128], visits;
static void inspect(void *ctx, unsigned slot, unsigned priority)
{
    struct ss_owner *o = ctx;
    assert(priority == 5); ++visited[slot]; ++visits;
    /* Replenishing the first word must not starve the detached second word. */
    ss_owner_publish(o, 0, 5);
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
    ss_owner_publish(&owner, 127, 7);
    ss_owner_put(&descriptor);
    return NULL;
}
int main(void)
{
    scan_costs();
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
    assert(!ss_owner_commit_dispatch(&owner, 6, false));
    ss_owner_publish(&owner, 9, 5); assert(!sgis);
    assert(ss_owner_commit_dispatch(&owner, 4, false));
    ss_owner_publish(&owner, 10, 5); assert(sgis==1);
    ss_owner_publish(&owner, 11, 5); assert(sgis==1);
    assert(ss_owner_commit_dispatch(&owner, 0, true));
    ss_owner_publish(&owner, 127, 5); assert(sgis==2);
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
    puts("scheduler owner: FIFO, bounded fairness, dispatch and retirement pass");
}
