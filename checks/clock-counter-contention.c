/* Host contention check for the platform counter clock. The model counter
 * follows CLOCK_MONOTONIC but advances 1000 times slower than real time, so
 * one modeled 100 microsecond wait lasts 100 ms. Host preemption of a lock
 * holder therefore cannot imitate an IRQ-masked holder that has stalled.
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
static _Atomic unsigned model_reads;
uint64_t a733_counter_model_level(void){return 4;}
uint64_t a733_counter_model_mask(void){return 0;}
void a733_counter_model_restore(uint64_t flags){(void)flags;}
uint64_t a733_counter_model_frequency(void){return atomic_load(&model_frequency);}
void a733_counter_model_relax(void){sched_yield();}
uint64_t a733_counter_model_ticks(void)
{
    atomic_fetch_add(&model_reads,1);
    uint64_t fixed=atomic_load(&model_override);if(fixed)return fixed;
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    uint64_t ns=(uint64_t)t.tv_sec*1000000000u+(uint64_t)t.tv_nsec;
    return ns*24u/1000000u+(UINT64_C(1)<<32)+(uint64_t)atomic_load(&model_offset);
}
static struct a733_counter_clock shared;
static unsigned calls_per_thread=200000;
static void *worker(void *unused)
{
    (void)unused;uint64_t last=0,now;
    for(unsigned i=0;i<calls_per_thread;++i) {
        assert(a733_counter_clock_now(&shared,&now)==1);
        assert(now>=last);last=now;
    }
    return NULL;
}
static double seconds(void)
{struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
int main(int argc,char **argv)
{
    unsigned threads=argc>1?(unsigned)strtoul(argv[1],NULL,10):16;
    if(argc>2)calls_per_thread=(unsigned)strtoul(argv[2],NULL,10);
    assert(threads>=1 && threads<=64);
    /* Contended monotonic input never faults, and each caller sees a
     * nondecreasing sequence. */
    assert(a733_counter_clock_init(&shared,24000000));
    pthread_t id[64];
    for(unsigned i=0;i<threads;++i)assert(!pthread_create(&id[i],NULL,worker,NULL));
    for(unsigned i=0;i<threads;++i)assert(!pthread_join(id[i],NULL));
    assert(!atomic_load(&shared.failed) && atomic_load(&shared.next)==atomic_load(&shared.serving));
    /* A regression against the shared history still latches the fault. */
    uint64_t value=0,kept=12345;
    assert(a733_counter_clock_now(&shared,&value)==1);
    atomic_store(&model_offset,-(int64_t)1000000);
    assert(a733_counter_clock_now(&shared,&kept)==0 && kept==12345 && atomic_load(&shared.failed));
    atomic_store(&model_offset,0);
    assert(a733_counter_clock_now(&shared,&kept)==0 && kept==12345);
    /* A changed CNTFRQ latches the fault. */
    struct a733_counter_clock c={0};
    assert(a733_counter_clock_init(&c,24000000) && a733_counter_clock_now(&c,&value)==1);
    atomic_store(&model_frequency,25000000);
    assert(a733_counter_clock_now(&c,&value)==0 && atomic_load(&c.failed));
    atomic_store(&model_frequency,24000000);
    /* A stalled holder ends the wait after 100 modeled microseconds of counter
     * time, independent of the number of attempts. */
    struct a733_counter_clock stalled={0};
    assert(a733_counter_clock_init(&stalled,24000000));
    atomic_store(&stalled.next,1); /* Ticket 0 is never served. */atomic_store(&model_reads,0);
    double begin=seconds();
    assert(a733_counter_clock_now(&stalled,&value)==0 && atomic_load(&stalled.failed));
    double waited=seconds()-begin;
    assert(waited>=0.099 && waited<5.0 && atomic_load(&model_reads)>2);
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
    printf("PASS counter clock: %u threads x %u calls without fault; regression, CNTFRQ change and %.0f ms stalled holder latch\n",
        threads,calls_per_thread,waited*1000);
    return 0;
}
