/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../kernel/scheduler/notification.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>

static void host_wait(void *unused) { (void)unused; sched_yield(); }
static const struct ss_owner_backend backend = {.wait=host_wait};
static struct ss_notification note;
static _Atomic unsigned start, gated;
static void *increment(void *unused)
{
    (void)unused;
    while (!atomic_load(&start)) sched_yield();
    for (unsigned i=0; i<10000; ++i) {
        struct ss_notification_result r = ss_notification_apply(&note,SS_NOTIFY_INCREMENT,0,&backend);
        assert(r.accepted);
        if (r.used_gate) atomic_fetch_add(&gated,1);
    }
    return NULL;
}
static _Atomic bool query_done;
static void *query(void *unused)
{
    (void)unused;
    struct ss_notification_result r = ss_notification_apply(&note,SS_NOTIFY_QUERY,0,&backend);
    assert(r.previous==1234); atomic_store(&query_done,true); return NULL;
}
int main(void)
{
    ss_notification_init(&note,0);
    struct ss_notification_guard g;
    struct ss_notification_wait w = {0};
    ss_notification_gate_begin(&note,&backend,&g);
    assert(ss_notification_wait_enter(&g,&w,1,SS_NOTIFY_WAIT_STATE,0,true));
    ss_notification_gate_end(&g);
    struct ss_notification_result r = ss_notification_apply(&note,SS_NOTIFY_NO_OVERWRITE,42,&backend);
    assert(r.accepted && r.completed_wait && r.previous_state==SS_NOTIFY_WAITING);
    r=ss_notification_apply(&note,SS_NOTIFY_NO_OVERWRITE,99,&backend);
    assert(!r.accepted && r.previous==42);
    ss_notification_apply(&note,SS_NOTIFY_CLEAR_STATE,0,&backend);
    ss_notification_apply(&note,SS_NOTIFY_CLEAR_BITS,UINT32_MAX,&backend);
    assert(atomic_load(&note.word)&SS_NOTIFICATION_COMPLETED);
    ss_notification_gate_begin(&note,&backend,&g);
    assert(ss_notification_wait_observe(&g,&w,1));
    assert(!ss_notification_wait_observe(&g,&w,1));
    struct ss_notification_finish f=ss_notification_wait_finish(&g,&w,1,0,false,true);
    assert(f.matched && !f.condition && f.scheduler_completed);
    assert(ss_notification_wait_enter(&g,&w,2,SS_NOTIFY_WAIT_COUNT,0,true));
    assert(!(g.payload&SS_NOTIFICATION_COMPLETED));
    ss_notification_gate_end(&g);
    ss_notification_apply(&note,SS_NOTIFY_INCREMENT,0,&backend);
    ss_notification_apply(&note,SS_NOTIFY_SET_BITS,8,&backend);
    ss_notification_gate_begin(&note,&backend,&g);
    f=ss_notification_wait_finish(&g,&w,1,0,true,true);
    assert(!f.matched && w.active);
    f=ss_notification_wait_finish(&g,&w,2,0,true,true);
    assert(f.condition && f.value==9 && (uint32_t)g.payload==8);
    /* An unsuccessful zero-time poll must close its owner generation. */
    g.payload = 0;
    assert(!ss_notification_wait_enter(&g,&w,3,SS_NOTIFY_WAIT_STATE,0,false));
    f=ss_notification_wait_finish(&g,&w,3,0,false,true);
    assert(f.matched && !f.condition && !w.active && !w.blocked);
    assert(!ss_notification_wait_enter(&g,&w,4,SS_NOTIFY_WAIT_COUNT,0,false));
    f=ss_notification_wait_finish(&g,&w,4,0,true,true);
    assert(f.matched && !f.condition && !w.active && !w.blocked);
    g.payload = 8;
    ss_notification_gate_end(&g);
    r=ss_notification_apply(&note,SS_NOTIFY_NO_ACTION,0,&backend); assert(r.previous==8);
    r=ss_notification_apply(&note,SS_NOTIFY_OVERWRITE,UINT32_MAX,&backend); assert(r.previous==8);
    r=ss_notification_apply(&note,SS_NOTIFY_INCREMENT,0,&backend); assert(r.previous==UINT32_MAX);
    /* A registered observer cannot read either intermediate payload write. */
    ss_notification_gate_begin(&note,&backend,&g); g.payload=1234;
    pthread_t observer; assert(!pthread_create(&observer,NULL,query,NULL));
    while (((atomic_load(&note.word)&SS_NOTIFICATION_GATE)/SS_NOTIFICATION_GATE_UNIT)<2)
        sched_yield();
    assert(!atomic_load(&query_done));
    ss_notification_gate_end(&g); assert(!pthread_join(observer,NULL));
    assert(!atomic_load(&note.next_ticket) || atomic_load(&note.next_ticket)==atomic_load(&note.serving));
    ss_notification_init(&note,0);
    pthread_t workers[8];
    for (unsigned i=0;i<8;++i) assert(!pthread_create(&workers[i],NULL,increment,NULL));
    atomic_store(&start,1);
    for (unsigned i=0;i<8;++i) assert(!pthread_join(workers[i],NULL));
    assert((uint32_t)atomic_load(&note.word)==80000);
    assert(!(atomic_load(&note.word)&SS_NOTIFICATION_GATE));
    assert(atomic_load(&note.next_ticket)==atomic_load(&note.serving));
    printf("scheduler notification: transforms, sticky wait, gated observer, 8-thread increments pass (%u gated)\n",atomic_load(&gated));
}
