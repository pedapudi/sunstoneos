/* Execute the production registry with the actors generated from session
 * grants. Delays stand for owner publications, not modeled hardware effects. */
#include "quiesce.h"
#include "cpu-return-step.h"
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int requested, failing;
int os_cpu_return_requested(void) { return requested; }
int os_cpu_return_failing(void) { return failing; }

struct actor {
    struct os_quiesce step;
    unsigned calls, finished, notes, observations;
};
static enum os_quiesce_result poll_owner(struct os_quiesce *);
#define ACTOR(NAME, ORDER) {{.owner=OS_QUIESCE_##NAME, .order=OS_QUIESCE_##ORDER, .step=poll_owner}, 0, 0, 0, 0},
static struct actor actors[] = {
#include "actors.inc"
};
#undef ACTOR
#define COUNT (sizeof actors / sizeof actors[0])
static unsigned now_us, delay_us, target, mode;
static unsigned notes, late_fault;
/* The instrumented registry calls this immediately after each raw request
 * store. An owner may observe that store before the class release. */
void os_quiesce_publication_probe(struct os_quiesce *q)
{
    static unsigned seen[OS_QUIESCE_TERMINALS+1];
    if(mode!=7)return;
    unsigned peers=0;
    for(unsigned i=0;i<COUNT;++i)peers+=actors[i].step.order==q->order;
    if(seen[q->order]++>=peers)return; /* Only the first publication pass. */
    for(unsigned i=0;i<COUNT;++i)
        if(actors[i].step.order==q->order)assert(!os_quiesce_requested(&actors[i].step));
}
/* Modes: 0 normal, 1 failure, 2 stalled owner, 3 one fault, 4 every owner
 * faults, 5 owner completes concurrently with a PENDING callback result,
 * 6 a later class faults while an earlier owner is pending, 7 an owner
 * observes request publication, 8 a fault arrives at the timeout boundary,
 * 9 a completed owner publishes a late fault, 10 its observer returns FAULT. */
static enum os_quiesce_result poll_owner(struct os_quiesce *q)
{
    struct actor *a=(void *)((char *)q - offsetof(struct actor,step));
    unsigned index=(unsigned)(a-actors);
    assert(os_quiesce_requested(q));
    assert(os_quiesce_result(q)!=OS_QUIESCE_FAULT);
    if(a->finished){
        ++a->observations;
        if((mode==9||mode==10)&&index==target){
            assert(os_quiesce_result(&actors[late_fault].step)==OS_QUIESCE_PENDING);
            if(mode==9)
                assert(os_quiesce_fault(q,0x80007000u|index,0xabc000u|index,(void *)(uintptr_t)(0x1000+index)));
            return OS_QUIESCE_FAULT;
        }
        return OS_QUIESCE_DONE;
    }
    ++a->calls; /* Hardware work never repeats after the stopped proof. */
    for(unsigned i=0;i<COUNT;++i)
        if(actors[i].step.order==q->order)assert(os_quiesce_requested(&actors[i].step));
    for(unsigned i=0;i<COUNT;++i)
        if(actors[i].step.order<q->order)
            assert(os_quiesce_result(&actors[i].step)!=OS_QUIESCE_PENDING);
    if(q->order==OS_QUIESCE_TERMINALS && !os_quiesce_drained_before(OS_QUIESCE_TERMINALS))
        return OS_QUIESCE_FAULT;
    if((mode==3 && index==target) || (mode==4 && q->order!=OS_QUIESCE_TERMINALS)) {
        assert(os_quiesce_fault(q,0x80007000u|index,0xabc000u|index,(void *)(uintptr_t)(0x1000+index)));
        os_quiesce_complete(q);
        assert(os_quiesce_result(q)==OS_QUIESCE_FAULT);
        /* A repeated error must preserve this owner's original code. */
        assert(os_quiesce_fault(q,0x80007999u,0xffffffffu,NULL));
        return OS_QUIESCE_FAULT;
    }
    if((mode==2||mode==6||mode==8) && index==target)return OS_QUIESCE_PENDING;
    if((mode==9||mode==10)&&index==late_fault&&now_us<100000)return OS_QUIESCE_PENDING;
    if(now_us < delay_us + (q->order+1)*10000u)return OS_QUIESCE_PENDING;
    a->finished=1;
    if(mode==5){os_quiesce_complete(q);return OS_QUIESCE_PENDING;}
    return OS_QUIESCE_DONE;
}
static void note(uint32_t site,uint32_t value,const void *caller)
{
    unsigned i=site&0xfffu;
    assert(i<COUNT && site==(0x8700c000u|(unsigned)actors[i].step.owner<<16|i));
    assert(value==(0xabc000u|i) && caller==(void *)(uintptr_t)(0x1000+i));
    assert(!actors[i].notes++);++notes;
}
int main(int argc,char **argv)
{
    assert(argc==4);
    mode=(unsigned)strtoul(argv[1],NULL,0);
    target=(unsigned)strtoul(argv[2],NULL,0);
    delay_us=(unsigned)strtoul(argv[3],NULL,0);
    assert(target<COUNT);
    /* Reverse registration order must not change class ordering. */
    for(unsigned i=0;i<COUNT;++i)assert(os_quiesce_register(&actors[i].step));
    assert(!os_quiesce_register(&actors[0].step));
    assert(os_quiesce_step(note)==OS_QUIESCE_PENDING);
    for(unsigned i=0;i<COUNT;++i)assert(!actors[i].calls);
    failing=mode!=0;requested=1;
    struct os_quiesce late={.owner=OS_QUIESCE_NPU,.order=OS_QUIESCE_QUEUES,.step=poll_owner};
    assert(!os_quiesce_register(&late));
    if(mode==6||mode==8||mode==9||mode==10){
        late_fault=COUNT;
        for(unsigned i=0;i<COUNT;++i)
            if(actors[i].step.order>actors[target].step.order&&actors[i].step.order!=OS_QUIESCE_TERMINALS){late_fault=i;break;}
        assert(late_fault<COUNT);
    }
    enum os_quiesce_result result=OS_QUIESCE_PENDING;
    for(now_us=0;!os_cpu_return_expired(now_us,0,RETURN_LIMIT_US);now_us+=10000){
        if(mode==6&&now_us==10000){
            assert(!os_quiesce_requested(&actors[late_fault].step));
            assert(os_quiesce_fault(&actors[late_fault].step,0x80007000u|late_fault,
                0xabc000u|late_fault,(void *)(uintptr_t)(0x1000+late_fault)));
        }
        result=os_quiesce_step(note);
        if(mode==6&&now_us>=10000){
            assert(notes==1&&actors[late_fault].notes==1);
            assert(!actors[late_fault].calls&&!os_quiesce_requested(&actors[late_fault].step));
        }
        if(result==OS_QUIESCE_DONE)break;
    }
    if(mode==8){
        assert(notes==0&&now_us==RETURN_LIMIT_US);
        assert(os_quiesce_fault(&actors[late_fault].step,0x80007000u|late_fault,
            0xabc000u|late_fault,(void *)(uintptr_t)(0x1000+late_fault)));
        os_quiesce_note_faults(note); /* The supervisor flushes before its stop. */
        assert(notes==1&&actors[late_fault].notes==1&&!actors[late_fault].calls);
    }
    if(mode==2||mode==6||mode==8){
        assert(now_us==RETURN_LIMIT_US && result!=OS_QUIESCE_DONE);
        for(unsigned i=0;i<COUNT;++i)
            if(actors[i].step.order>actors[target].step.order)assert(!actors[i].calls);
    }else if(mode==9||mode==10){
        assert(now_us==RETURN_LIMIT_US && result!=OS_QUIESCE_DONE);
        assert(actors[target].finished&&actors[target].observations==1);
        assert(os_quiesce_result(&actors[target].step)==OS_QUIESCE_FAULT);
        assert(notes==(mode==9?1u:0u));
        assert(actors[late_fault].finished);
        for(unsigned i=0;i<COUNT;++i)
            if(actors[i].step.order==OS_QUIESCE_TERMINALS)assert(!actors[i].finished);
    }else if(mode==3||mode==4){
        assert(now_us==RETURN_LIMIT_US && result!=OS_QUIESCE_DONE);
        assert(notes==(mode==3?1u:COUNT-1u));
        for(unsigned i=0;i<COUNT;++i)assert(actors[i].calls);
        assert(!os_quiesce_drained_before(OS_QUIESCE_TERMINALS));
    }else{
        assert(result==OS_QUIESCE_DONE && now_us<delay_us+200000u && !notes);
        for(unsigned i=0;i<COUNT;++i)assert(actors[i].finished);
        unsigned total=0,observations=0;
        for(unsigned i=0;i<COUNT;++i){total+=actors[i].calls;observations+=actors[i].observations;}
        assert(os_quiesce_step(note)==OS_QUIESCE_DONE);
        unsigned after=0,observed=0;
        for(unsigned i=0;i<COUNT;++i){after+=actors[i].calls;observed+=actors[i].observations;}
        assert(total==after&&observed==observations+COUNT);
    }
    assert(os_cpu_return_expired(RETURN_LIMIT_US,0,RETURN_LIMIT_US));
    assert(!os_cpu_return_expired(RETURN_LIMIT_US-1,0,RETURN_LIMIT_US));
    assert(os_cpu_return_bounded_detail(0,100,60000000,60000000,0,0)==0);
    assert(os_cpu_return_bounded_detail(1,100,60000000,60000000,0,0)==OS_BOUNDED_FATAL);
    assert(os_cpu_return_bounded_detail(1,100,60000000,60000000,0,0x28b)==(OS_BOUNDED_FATAL_NPU|0x28b));
    assert(os_cpu_return_bounded_detail(0,60000000,60000000,60000000,0,0)==OS_BOUNDED_DEADLINE);
    return 0;
}
