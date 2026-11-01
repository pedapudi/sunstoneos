/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../kernel/scheduler/owner.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>

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
