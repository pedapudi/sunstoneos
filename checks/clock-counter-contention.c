/* Host contention check for the platform counter clock. The model counter
 * follows CLOCK_MONOTONIC but advances 1000 times slower than real time, so
 * 100 modeled microseconds last 100 ms. Each thread may add its own fixed
 * offset to model the skew between cores' counter reads.
 * Build with -DSUNSTONE_COUNTER_MODEL and platform/a733/counter_clock.c. */
#define _GNU_SOURCE
#include "a733_counter_clock.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static _Atomic uint64_t model_frequency=24000000,model_override;
static _Atomic int64_t model_offset;
static _Thread_local int64_t thread_skew;
static _Thread_local int stall_after_sample;
static _Atomic int stalled,resume;
uint64_t a733_counter_model_level(void){return 4;}
uint64_t a733_counter_model_frequency(void){return atomic_load(&model_frequency);}
uint64_t a733_counter_model_ticks(void)
{
    uint64_t fixed=atomic_load(&model_override);if(fixed)return fixed;
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    uint64_t ns=(uint64_t)t.tv_sec*1000000000u+(uint64_t)t.tv_nsec;
    uint64_t ticks=ns*24u/1000000u+(UINT64_C(1)<<32)+(uint64_t)atomic_load(&model_offset)+(uint64_t)thread_skew;
    /* A caller that stops between its counter sample and its history update. */
    if(stall_after_sample) {
        stall_after_sample=0;atomic_store(&stalled,1);
        while(!atomic_load(&resume))sched_yield();
    }
    return ticks;
}
static struct a733_counter_clock shared;
static unsigned calls_per_thread=200000;
struct worker_args {int64_t skew;unsigned calls;};
static void *worker(void *argument)
{
    const struct worker_args *a=argument;thread_skew=a->skew;uint64_t last=0,now;
    for(unsigned i=0;i<a->calls;++i) {
        assert(a733_counter_clock_now(&shared,&now)==1);
        assert(now>=last);last=now;
    }
    return NULL;
}
static void *stalled_caller(void *result)
{
    stall_after_sample=1;
    assert(a733_counter_clock_now(&shared,result)==1);
    return NULL;
}
static double seconds(void)
{struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
/* Runs threads callers concurrently; thread i adds skews[i % count] ticks. */
static double contend(unsigned threads,const int64_t *skews,unsigned count)
{
    pthread_t id[64];struct worker_args args[64];double begin=seconds();
    for(unsigned i=0;i<threads;++i) {
        args[i]=(struct worker_args){skews[i%count],calls_per_thread};
        assert(!pthread_create(&id[i],NULL,worker,&args[i]));
    }
    for(unsigned i=0;i<threads;++i)assert(!pthread_join(id[i],NULL));
    assert(!atomic_load(&shared.failed));
    return (seconds()-begin)*1e9/((double)threads*calls_per_thread);
}
int main(int argc,char **argv)
{
    unsigned threads=argc>1?(unsigned)strtoul(argv[1],NULL,10):16;
    if(argc>2)calls_per_thread=(unsigned)strtoul(argv[2],NULL,10);
    assert(threads>=2 && threads<=64);
    /* Contended monotonic input never faults, and each caller sees a
     * nondecreasing sequence. */
    assert(a733_counter_clock_init(&shared,24000000));
    const int64_t aligned[]={0};
    const double aligned_ns=contend(threads,aligned,1);
    /* Counters up to 21 ticks (0.9 microseconds) apart still never fault, and
     * each caller still sees a nondecreasing sequence. */
    const int64_t skewed[]={0,-3,-6,-9,-12,-15,-18,-21};
    const double skewed_ns=contend(threads,skewed,8);
    /* A caller that stops after its counter sample delays and faults no other
     * caller. Resumed, it returns time no earlier than any completed call. */
    uint64_t late=0,before=0;
    pthread_t holder;assert(!pthread_create(&holder,NULL,stalled_caller,&late));
    while(!atomic_load(&stalled))sched_yield();
    contend(threads,aligned,1);
    assert(a733_counter_clock_now(&shared,&before)==1);
    atomic_store(&resume,1);assert(!pthread_join(holder,NULL));
    assert(late>=before && !atomic_load(&shared.failed));
    /* A regression against the shared history still latches the fault. */
    uint64_t value=0,kept=12345;
    assert(a733_counter_clock_now(&shared,&value)==1);
    atomic_store(&model_offset,-(int64_t)1000000);
    assert(a733_counter_clock_now(&shared,&kept)==0 && kept==12345 && atomic_load(&shared.failed));
    atomic_store(&model_offset,0);
    assert(a733_counter_clock_now(&shared,&kept)==0 && kept==12345);
    /* The skew bound is 100 microseconds plus one tick: 2401 ticks at 24 MHz
     * report the history, and 2402 ticks fault. */
    struct a733_counter_clock bound={0};
    atomic_store(&model_override,UINT64_C(1)<<32);
    assert(a733_counter_clock_init(&bound,24000000));
    atomic_store(&model_override,(UINT64_C(1)<<32)+24000);
    assert(a733_counter_clock_now(&bound,&value)==1 && value==((UINT64_C(1)<<32)+24000)/24);
    atomic_store(&model_override,(UINT64_C(1)<<32)+24000-2401);
    assert(a733_counter_clock_now(&bound,&kept)==1 && kept==value && !atomic_load(&bound.failed));
    atomic_store(&model_override,(UINT64_C(1)<<32)+24000-2402);
    assert(a733_counter_clock_now(&bound,&kept)==0 && atomic_load(&bound.failed));
    atomic_store(&model_override,0);
    /* A changed CNTFRQ latches the fault. */
    struct a733_counter_clock c={0};
    assert(a733_counter_clock_init(&c,24000000) && a733_counter_clock_now(&c,&value)==1);
    atomic_store(&model_frequency,25000000);
    assert(a733_counter_clock_now(&c,&value)==0 && atomic_load(&c.failed));
    atomic_store(&model_frequency,24000000);
    /* The constant 24 MHz conversion equals the checked general form. */
    const uint64_t samples[]={UINT64_C(1)<<33,UINT64_C(23999999)+(UINT64_C(1)<<40),UINT64_MAX/2,UINT64_MAX-1};
    for(unsigned i=0;i<sizeof(samples)/sizeof(samples[0]);++i) {
        struct a733_counter_clock fixed={0};
        atomic_store(&model_override,UINT64_C(1)<<32);
        assert(a733_counter_clock_init(&fixed,24000000));
        atomic_store(&model_override,samples[i]);
        assert(a733_counter_clock_now(&fixed,&value)==1);
        uint64_t whole=(samples[i]/24000000u)*1000000u,fraction=(samples[i]%24000000u)*1000000u/24000000u;
        assert(value==whole+fraction);
    }
    /* Another admitted rate keeps the checked conversion. */
    struct a733_counter_clock other={0};
    atomic_store(&model_override,UINT64_C(625000000));atomic_store(&model_frequency,62500000);
    assert(a733_counter_clock_init(&other,62500000) && a733_counter_clock_now(&other,&value)==1 && value==10000000);
    atomic_store(&model_override,UINT64_MAX);
    assert(a733_counter_clock_now(&other,&value)==1 && value==UINT64_MAX/62500000u*1000000u+(UINT64_MAX%62500000u)*1000000u/62500000u);
    printf("PASS counter clock: %u threads x %u calls without fault, %.0f ns per call aligned and %.0f ns with skew; "
        "a stalled caller blocks no other; regression, skew bound and CNTFRQ change latch\n",
        threads,calls_per_thread,aligned_ns,skewed_ns);
    return 0;
}
