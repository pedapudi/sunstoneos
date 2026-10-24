/* Display-port latency window, failure latch, monotonic counter rule,
 * phase spans, copy-load stop rule and evidence block against a counting
 * model of port 2's counters. Built by
 * checks/dram-controller.py with drivers/dram/a733_dram.c and
 * drivers/dram/a733_dram_load.c. */
#include "a733_dram.h"
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define HZ UINT64_C(24000000)
#define P (HZ/20)
#define FABRIC 600000000u
#define PORT A733_NSI_PORT(2)
enum { HOLD, FREE };
struct model {
    uint64_t now,step;          /* Counter value; added by every counter read. */
    uint32_t enable,clear,cycle;
    uint32_t rq_rd,rq_wr,dt_rd,dt_wr,la_rd; /* Values the counters hold after a window. */
    unsigned behaviour,held,pairs,busy,fail_at,enter_fails,back_at;
    unsigned reads,writes,counters,accesses,enters;
    uintptr_t log[16];uint32_t values[16];int written[16];
};
static uint32_t *reg(struct model *m,uintptr_t a)
{
    assert(a733_dram_window_address(a));
    switch(a-PORT){
    case 0xc0:return &m->enable;case 0xc4:return &m->clear;case 0xc8:return &m->cycle;
    case 0xcc:return &m->rq_rd;case 0xd0:return &m->rq_wr;case 0xd4:return &m->dt_rd;
    case 0xd8:return &m->dt_wr;case 0xdc:return &m->la_rd;
    default:assert(!"unexpected counter word");return NULL;
    }
}
static int rd(void *v,uintptr_t a,uint32_t *out)
{
    struct model *m=v;assert(m->held);
    if(m->accesses<16){m->log[m->accesses]=a;m->written[m->accesses]=0;}
    if(++m->accesses==m->fail_at)return 0;
    ++m->reads;*out=*reg(m,a);
    /* Free-running counters keep counting while they are enabled. */
    if(m->behaviour==FREE&&a==PORT+0xd4&&(m->enable&1))m->dt_rd+=64;
    return 1;
}
static int wr(void *v,uintptr_t a,uint32_t value)
{
    struct model *m=v;assert(m->held);
    if(m->accesses<16){m->log[m->accesses]=a;m->values[m->accesses]=value;m->written[m->accesses]=1;}
    if(++m->accesses==m->fail_at)return 0;
    ++m->writes;*reg(m,a)=value;return 1;
}
static int enter(void *v)
{
    struct model *m=v;assert(!m->held);++m->enters;
    if(m->enter_fails)return 2;
    if(m->busy)return 0;
    m->held=1;++m->pairs;return 1;
}
static void leave(void *v){struct model *m=v;assert(m->held);m->held=0;}
/* back_at: the counter read, counted from counts(), that returns a value
 * earlier than the latest one. */
static uint64_t counter(void *v)
{
    struct model *m=v;
    if(++m->counters==m->back_at)return m->now-10;
    m->now+=m->step;return m->now;
}
static struct model m;
static struct a733_dram_window w;
static void counts(void){m.reads=m.writes=m.counters=m.pairs=m.accesses=m.enters=0;}
static void setup(uint64_t end)
{
    memset(&m,0,sizeof(m));memset(&w,0,sizeof(w));
    m.now=UINT64_C(1)<<40;m.enable=0xa0;m.clear=0x50;
    m.rq_rd=4000;m.dt_rd=20000;m.la_rd=4000*300;
    struct a733_cpu_clock_report r={0};
    r.domain[A733_CPU_LITTLE]=(struct a733_cpu_clock_domain){.hz=1794000000,.decoded=1};
    r.domain[A733_CPU_BIG]=(struct a733_cpu_clock_domain){.hz=2002000000,.decoded=1};
    const struct a733_dram_window_ops ops={{&m,rd,wr},enter,leave,counter};
    assert(a733_dram_window_init(&w,&ops,HZ,FABRIC,m.now+end,&r));
    assert(w.mhz[0]==1794 && w.mhz[1]==2002 && w.mhz[2]==0);
    counts();
}
/* Run one window from start to collect; returns the step result of collect. */
static int window(uint64_t wait)
{
    assert(!w.in_flight);
    counts();assert(a733_dram_window_step(&w,1)==0 && w.in_flight);
    m.now+=wait;counts();
    int r=a733_dram_window_step(&w,1);
    return r;
}
int main(void)
{
    /* Initialization refusals: rates that are not multiples of 20, a fabric
     * above 600 MHz and missing callbacks. */
    {
        struct a733_dram_window x;struct a733_cpu_clock_report r={0};
        const struct a733_dram_window_ops ops={{&m,rd,wr},enter,leave,counter},none={{&m,rd,wr},enter,leave,NULL};
        memset(&x,0,sizeof(x));assert(!a733_dram_window_init(&x,&ops,HZ+1,FABRIC,1,&r));
        memset(&x,0,sizeof(x));assert(!a733_dram_window_init(&x,&ops,HZ,FABRIC+20,1,&r));
        memset(&x,0,sizeof(x));assert(!a733_dram_window_init(&x,&ops,HZ,FABRIC-10,1,&r));
        memset(&x,0,sizeof(x));assert(!a733_dram_window_init(&x,&none,HZ,FABRIC,1,&r));
        memset(&x,0,sizeof(x));assert(!a733_dram_window_init(&x,&ops,HZ,FABRIC,1,NULL));
        memset(&x,0,sizeof(x));assert(a733_dram_window_init(&x,&ops,HZ,FABRIC,1,&r) &&
            !a733_dram_window_init(&x,&ops,HZ,FABRIC,1,&r));
    }
    /* Counter words: +0xc0..+0xdc of port 2 only. */
    unsigned admitted=0;
    for(uintptr_t a=A733_NSI_BASE;a<A733_NSI_BASE+0x10000;a+=4)admitted+=(unsigned)a733_dram_window_address(a);
    assert(admitted==8 && !a733_dram_window_address(PORT+0xe0) && !a733_dram_window_address(PORT+0xc1) &&
        !a733_dram_window_address(A733_NSI_PORT(14)+0xc0));

    /* Not eligible (stage 2, state 0, fewer than three presentations): no
     * access and no counter read. */
    setup(18*HZ);
    for(unsigned i=0;i<3;++i)assert(a733_dram_window_step(&w,0)==0);
    assert(!m.counters && !m.accesses && !m.pairs);

    /* Start: period, disable, clear, enable with the saved word; one lock
     * pair, two counter reads and no barrier of its own. */
    assert(a733_dram_window_step(&w,1)==0 && w.in_flight);
    assert(m.accesses==6 && m.pairs==1 && m.counters==2 && m.writes==4 && m.reads==2);
    static const uint32_t offsets[6]={0xc8,0xc0,0xc0,0xc4,0xc4,0xc0};
    static const int writes[6]={1,0,1,0,1,1};
    for(unsigned i=0;i<6;++i)assert(m.log[i]==PORT+offsets[i] && m.written[i]==writes[i]);
    uintptr_t start_log[6];int start_written[6];
    memcpy(start_log,m.log,sizeof(start_log));memcpy(start_written,m.written,sizeof(start_written));
    assert(m.values[0]==FABRIC/20 && m.values[0]==30000000u); /* P x fabric_hz. */
    assert(m.values[2]==0xa0 && m.values[4]==0x51 && m.values[5]==0xa1 && m.cycle==30000000u);
    /* Pending: one counter read, nothing else. */
    for(unsigned i=0;i<5;++i){counts();m.now+=P/10;assert(a733_dram_window_step(&w,1)==0);
        assert(m.counters==1 && !m.accesses && !m.pairs);}
    /* Collect at t1 + P: six reads, read data twice; one pair and one
     * counter read after the pending check's. */
    m.now=w.t1+P;counts();assert(a733_dram_window_step(&w,1)==0 && !w.in_flight);
    assert(m.accesses==6 && m.reads==6 && !m.writes && m.pairs==1 && m.counters==2);
    static const uint32_t reads[6]={0xcc,0xd0,0xd4,0xd8,0xdc,0xd4};
    for(unsigned i=0;i<6;++i)assert(m.log[i]==PORT+reads[i] && !m.written[i]);
    uintptr_t collect_log[6];memcpy(collect_log,m.log,sizeof(collect_log));
    assert(w.count[A733_DRAM_STARTED]==1 && w.count[A733_DRAM_ACCEPTED_A]==1);
    assert(w.sum[0]==300*256 && w.square[0]==(uint64_t)(300*256)*(300*256) && w.data[0]==20000);

    /* Refusals. Late: the reads came after 2P from t0. */
    assert(window(2*P)==0 && w.count[A733_DRAM_LATE]==1);
    /* Idle: no read requests, or read data below 1,024 units. */
    m.rq_rd=0;assert(window(P)==0 && w.count[A733_DRAM_IDLE_REFUSED]==1);
    m.rq_rd=4000;m.dt_rd=1023;assert(window(P)==0 && w.count[A733_DRAM_IDLE_REFUSED]==2);
    m.dt_rd=1024;assert(window(P)==0 && w.count[A733_DRAM_ACCEPTED_A]==2);
    /* Out of range: x >= 2^24 is counted, not summed. */
    uint64_t sum=w.sum[0];
    m.rq_rd=1;m.la_rd=1u<<16;assert(window(P)==0 && w.count[A733_DRAM_OUT_OF_RANGE]==1 && w.sum[0]==sum);
    m.la_rd=(1u<<16)-1;assert(window(P)==0 && w.count[A733_DRAM_ACCEPTED_A]==3 && w.sum[0]==sum+((1u<<24)-256));
    m.rq_rd=4000;m.la_rd=4000*300;
    assert(!w.fallback);

    /* Free-running counters: the first refusal switches every later collect
     * to disable first, and the disabled counters then hold. */
    setup(18*HZ);m.behaviour=FREE;
    assert(window(P)==0 && m.accesses==6 && !m.writes);
    assert(w.count[A733_DRAM_FREE_RUNNING]==1 && w.fallback);
    assert(window(P)==0 && m.accesses==7 && m.writes==1 && m.log[0]==PORT+0xc0 && m.values[0]==0xa0);
    assert(w.count[A733_DRAM_ACCEPTED_A]==1 && w.count[A733_DRAM_FREE_RUNNING]==1);
    /* One free-running window among held ones engages the fallback too. */
    setup(18*HZ);
    for(unsigned i=0;i<5;++i)assert(window(P)==0 && m.accesses==6 && !w.fallback);
    m.behaviour=FREE;assert(window(P)==0 && w.fallback && w.count[A733_DRAM_FREE_RUNNING]==1);
    m.behaviour=HOLD;assert(window(P)==0 && m.accesses==7 && w.count[A733_DRAM_ACCEPTED_A]==6);

    /* A busy lock changes nothing and the next step retries; a failed access
     * returns failure without leaving, for the caller's retain path. */
    setup(18*HZ);m.busy=1;assert(a733_dram_window_step(&w,1)==0 && !w.in_flight && !m.accesses);
    m.busy=0;assert(a733_dram_window_step(&w,1)==0 && w.in_flight);
    m.now+=P;m.busy=1;assert(a733_dram_window_step(&w,1)==0 && w.in_flight && !w.count[A733_DRAM_ACCEPTED_A]);
    m.busy=0;assert(a733_dram_window_step(&w,1)==0 && !w.in_flight && w.count[A733_DRAM_ACCEPTED_A]==1);
    /* After a failed access or enter, nothing calls enter, the I/O pair or
     * the counter again: later steps, eligible or not, and abandon return at
     * once, and the copy tasks see the end phase. */
    unsigned after_failure=0;
    struct a733_dram_copy_slot idle;atomic_init(&idle.passes,0);atomic_uint running;atomic_init(&running,1);
    uint8_t byte=0;
    for(unsigned k=1;k<=8;++k)for(unsigned in_collect=0;in_collect<2;++in_collect){
        if(k==8&&in_collect)continue;
        setup(18*HZ);
        if(in_collect){assert(a733_dram_window_step(&w,1)==0);m.now+=P;counts();}
        if(k==7)m.enter_fails=1;
        else if(k==8){m.behaviour=FREE;assert(window(P)==0 && w.fallback);m.behaviour=HOLD;
            assert(a733_dram_window_step(&w,1)==0);m.now+=P;counts();m.fail_at=1;}
        else m.fail_at=k;
        assert(a733_dram_window_step(&w,1)<0);
        if(k==7)assert(!m.held && !m.accesses && m.enters==1);
        else assert(m.held && m.accesses==(k==8?1u:k));
        assert(w.failed && w.early && atomic_load(&w.phase)==A733_DRAM_ENDED);
        /* The lock stays held after a failed access, so an enter would assert. */
        m.enter_fails=0;m.fail_at=0;counts();
        for(unsigned i=0;i<3;++i){m.now+=P;assert(a733_dram_window_step(&w,i!=1)==0);}
        a733_dram_window_abandon(&w);
        assert(a733_dram_copy_pass(&w,&running,&idle,&byte,&byte)==0);
        assert(!m.accesses && !m.enters && !m.pairs && !m.counters);
        after_failure+=m.accesses+m.enters+m.counters;
    }
    {
        /* A failure during phase B ends it for the copy tasks with release;
         * no counter read records an end, so the block shows 0 ms. */
        setup(4*HZ);
        assert(a733_dram_window_step(&w,1)==0);m.now+=P;assert(a733_dram_window_step(&w,1)==0);
        m.now+=2*HZ;assert(a733_dram_window_step(&w,1)==A733_DRAM_BEGIN_B);
        assert(a733_dram_window_step(&w,1)==0);m.now+=P;counts();m.fail_at=3;
        assert(a733_dram_window_step(&w,1)<0 && m.held);counts();
        a733_dram_window_abandon(&w);assert(!m.counters && !w.b_end);
        struct a733_dram_priority_evidence e={0};struct a733_dram_block value;
        struct a733_dram_copy_slot none[8];for(unsigned i=0;i<8;++i)atomic_init(&none[i].passes,0);
        a733_dram_block(&e,1,&value);a733_dram_window_block(&w,none,&value);
        assert(value.result&A733_DRAM_ENDED_EARLY && !value.phase_b_ms);
    }

    /* A counter sample earlier than the latest one refuses the window as
     * late: at the pending check with no access, at t1 after the start's
     * accesses and at t_end before any sum changes. */
    setup(18*HZ);assert(a733_dram_window_step(&w,1)==0 && w.in_flight);
    counts();m.back_at=1;assert(a733_dram_window_step(&w,1)==0 && !w.in_flight);
    assert(!m.accesses && m.counters==1 && w.count[A733_DRAM_LATE]==1);
    counts();m.back_at=2;assert(a733_dram_window_step(&w,1)==0 && !w.in_flight);
    assert(m.accesses==6 && m.pairs==1 && w.count[A733_DRAM_STARTED]==2 && w.count[A733_DRAM_LATE]==2);
    m.back_at=0;assert(a733_dram_window_step(&w,1)==0 && w.in_flight);m.now+=P;
    counts();m.back_at=2;assert(a733_dram_window_step(&w,1)==0 && !w.in_flight);
    assert(m.accesses==6 && m.pairs==1 && w.count[A733_DRAM_LATE]==3 && !w.count[A733_DRAM_ACCEPTED_A] && !w.sum[0]);
    m.back_at=0;assert(window(P)==0 && w.count[A733_DRAM_ACCEPTED_A]==1);
    /* Not in flight: a backward sample starts nothing. */
    counts();m.back_at=1;assert(a733_dram_window_step(&w,1)==0 && !w.in_flight && !m.accesses);
    assert(w.count[A733_DRAM_LATE]==3);
    /* In phase B, abandon records no end from a backward sample. */
    setup(4*HZ);
    assert(a733_dram_window_step(&w,1)==0);m.now+=P;assert(a733_dram_window_step(&w,1)==0);
    m.now+=2*HZ;assert(a733_dram_window_step(&w,1)==A733_DRAM_BEGIN_B);
    counts();m.back_at=1;a733_dram_window_abandon(&w);assert(m.counters==1 && !w.b_end && w.early);

    /* phase_ticks_max: spans of 100 ticks, then 70,000, give 100 and then
     * 65,535. Start spans t1 - t0 and collect spans t_end minus the pending
     * sample; neither adds a counter read. A start of 100 ticks and a collect
     * of 300 show each span on its own. */
    setup(18*HZ);m.step=100;
    assert(a733_dram_window_step(&w,1)==0 && w.in_flight && w.ticks_max==100 && m.counters==2);
    m.now+=P;m.step=300;counts();
    assert(a733_dram_window_step(&w,1)==0 && w.count[A733_DRAM_ACCEPTED_A]==1 && w.ticks_max==300 && m.counters==2);
    m.step=100;assert(window(P)==0 && w.count[A733_DRAM_ACCEPTED_A]==2 && w.ticks_max==300);
    m.step=70000;assert(window(P)==0 && w.count[A733_DRAM_ACCEPTED_A]==3 && w.ticks_max==UINT16_MAX);
    {
        struct a733_dram_priority_evidence e={0};struct a733_dram_block value;
        struct a733_dram_copy_slot none[8];for(unsigned i=0;i<8;++i)atomic_init(&none[i].passes,0);
        a733_dram_block(&e,1,&value);assert(!value.phase_ticks_max);
        a733_dram_window_block(&w,none,&value);assert(value.phase_ticks_max==UINT16_MAX);
    }

    /* Abandon: eligibility lost, stop_at and the fault path make no access. */
    setup(18*HZ);assert(a733_dram_window_step(&w,1)==0 && w.in_flight);counts();
    assert(a733_dram_window_step(&w,0)==A733_DRAM_END && !w.in_flight && w.count[A733_DRAM_ABANDONED]==1);
    assert(!m.accesses && !m.counters && !m.pairs);
    assert(a733_dram_window_step(&w,1)==0 && !m.accesses && !m.counters); /* Ended: nothing starts. */
    setup(18*HZ);assert(a733_dram_window_step(&w,1)==0);counts();
    a733_dram_window_abandon(&w);assert(!m.accesses && !m.counters && w.count[A733_DRAM_ABANDONED]==1);
    a733_dram_window_abandon(&w);assert(w.count[A733_DRAM_ABANDONED]==1);
    assert(atomic_load(&w.phase)==A733_DRAM_ENDED && a733_dram_window_step(&w,1)==0 && !m.counters);
    assert(w.early && !w.b_end);
    /* An abandon in phase B makes no register access, reads the counter
     * once and records the end of phase B. */
    setup(4*HZ);
    assert(a733_dram_window_step(&w,1)==0 && w.in_flight); /* Phase A and its first window. */
    m.now+=P;assert(a733_dram_window_step(&w,1)==0 && !w.in_flight);
    m.now+=2*HZ;assert(a733_dram_window_step(&w,1)==A733_DRAM_BEGIN_B);
    assert(a733_dram_window_step(&w,1)==0 && w.in_flight);
    m.now+=P/2;counts();a733_dram_window_abandon(&w);
    assert(!m.accesses && !m.pairs && m.counters==1 && w.b_end==m.now && w.early);
    assert(w.count[A733_DRAM_ABANDONED]==1 && atomic_load(&w.phase)==A733_DRAM_ENDED);
    {
        struct a733_dram_priority_evidence e={0};struct a733_dram_block value;
        struct a733_dram_copy_slot none[8];for(unsigned i=0;i<8;++i)atomic_init(&none[i].passes,0);
        a733_dram_block(&e,1,&value);a733_dram_window_block(&w,none,&value);
        assert(value.result&A733_DRAM_ENDED_EARLY && value.phase_b_ms==(w.b_end-w.b_start)*1000/HZ && value.phase_b_ms>=25);
    }

    /* Copy load: no pass before phase B, after the end phase or in a state
     * other than 1. */
    {
        uint8_t *src=malloc(A733_DRAM_COPY_BYTES),*dst=malloc(A733_DRAM_COPY_BYTES);assert(src&&dst);
        memset(src,0x3c,A733_DRAM_COPY_BYTES);memset(dst,0,A733_DRAM_COPY_BYTES);
        struct a733_dram_copy_slot slot;atomic_init(&slot.passes,0);atomic_uint state;atomic_init(&state,1);
        assert(_Alignof(struct a733_dram_copy_slot)==64 && sizeof(struct a733_dram_copy_slot)==64);
        setup(18*HZ);
        assert(a733_dram_copy_pass(&w,&state,&slot,dst,src)==-1 && !dst[0]);
        assert(a733_dram_window_step(&w,1)==0);           /* Phase A. */
        assert(a733_dram_copy_pass(&w,&state,&slot,dst,src)==-1 && !dst[0]);
        atomic_store(&w.phase,A733_DRAM_PHASE_B);
        assert(a733_dram_copy_pass(&w,&state,&slot,dst,src)==1 && dst[0]==0x3c && dst[A733_DRAM_COPY_BYTES-1]==0x3c);
        assert(atomic_load(&slot.passes)==1);
        for(unsigned s=2;s<=3;++s){
            memset(dst,0,A733_DRAM_COPY_BYTES);atomic_store(&state,s);
            assert(a733_dram_copy_pass(&w,&state,&slot,dst,src)==0 && !dst[0] && atomic_load(&slot.passes)==1);
        }
        atomic_store(&state,1);a733_dram_window_abandon(&w);
        assert(a733_dram_copy_pass(&w,&state,&slot,dst,src)==0 && !dst[0] && atomic_load(&slot.passes)==1);
        free(src);free(dst);
    }

    /* Session: one step per 1 ms tick from the third presentation to
     * stop_at; phase A lasts 2 s and phase B ends 1 s before stop_at. The
     * phase-changing steps start no window and make no access. */
    setup(17*HZ);m.step=1; /* end = stop_at - 1 s with stop_at 18 s after the third presentation. */
    struct a733_dram_copy_slot slots[8];
    for(unsigned i=0;i<8;++i)atomic_init(&slots[i].passes,10*i+1);
    uint64_t stop=m.now+18*HZ;unsigned begin=0,end=0,max_pending=0,total=0;
    while(m.now<stop){
        counts();
        unsigned in_flight=w.in_flight;
        int r=a733_dram_window_step(&w,1);
        assert(r>=0);total+=m.accesses;
        if(r==A733_DRAM_BEGIN_B){++begin;assert(!m.accesses && !w.in_flight && !in_flight);}
        if(r==A733_DRAM_END){++end;assert(!m.accesses && !in_flight);}
        if(in_flight&&w.in_flight&&m.counters>max_pending)max_pending=m.counters;
        if(!in_flight&&w.in_flight)m.rq_rd=4000+(atomic_load(&w.phase)==A733_DRAM_PHASE_B)*1000;
        if(!in_flight&&w.in_flight)m.la_rd=m.rq_rd*(atomic_load(&w.phase)==A733_DRAM_PHASE_B?400:300);
        m.now+=HZ/1000;
    }
    a733_dram_window_abandon(&w);
    assert(begin==1 && end==1 && max_pending==1);
    unsigned a=w.count[A733_DRAM_ACCEPTED_A],b=w.count[A733_DRAM_ACCEPTED_B];
    assert(a>=38 && a<=40 && b>=294 && b<=300 && w.count[A733_DRAM_STARTED]==a+b);
    assert(w.sum[0]==(uint64_t)a*300*256 && w.sum[1]==(uint64_t)b*400*256);
    uint64_t ms=(w.b_end-w.b_start)*1000/HZ;assert(ms>=14990 && ms<=15000);

    /* Evidence block with the window and copy slots. */
    struct a733_dram_priority_evidence e={.nsi_clock=0xc3000002,.nsi_config=0x10001,
        .inherited={1,2,3},.final={0x4a,0x7e,0x7e}};
    w.fallback=1;assert(!w.early);
    struct a733_dram_block value;a733_dram_block(&e,1,&value);a733_dram_window_block(&w,slots,&value);
    const uint8_t *block=(const uint8_t *)&value;
    /* Offsets of docs/designs/dram-controller.md. */
    assert(offsetof(struct a733_dram_block,result)==16 && offsetof(struct a733_dram_block,inherited)==20 &&
        offsetof(struct a733_dram_block,mhz)==32 && offsetof(struct a733_dram_block,nsi_clock)==40 &&
        offsetof(struct a733_dram_block,fabric_hz)==48 && offsetof(struct a733_dram_block,phase)==56 &&
        offsetof(struct a733_dram_block,passes)==104 && sizeof(value)==136);
#define U16(o) ((uint32_t)block[o]|(uint32_t)block[(o)+1]<<8)
#define U32(o) (U16(o)|U16((o)+2)<<16)
#define U64(o) ((uint64_t)U32(o)|(uint64_t)U32((o)+4)<<32)
    for(unsigned i=0;i<A733_DRAM_COUNTS;++i)assert(U16(2*i)==w.count[i]);
    assert(U16(16)==15 && U16(18)==0);
    for(unsigned i=0;i<3;++i)assert(U16(20+2*i)==e.inherited[i] && U16(26+2*i)==e.final[i]);
    assert(U16(32)==1794 && U16(34)==2002 && U16(36)==0 && U16(38)==w.ticks_max && w.ticks_max==1);
    assert(U32(40)==0xc3000002 && U32(44)==0x10001 && U32(48)==FABRIC && U32(52)==ms);
    for(unsigned i=0;i<2;++i)assert(U64(56+24*i)==w.sum[i] && U64(64+24*i)==w.square[i] && U64(72+24*i)==w.data[i]);
    for(unsigned i=0;i<8;++i)assert(U32(104+4*i)==10*i+1);
    printf("{\"start_accesses\":6,\"collect_accesses\":6,\"collect_after_fallback\":7,"
        "\"pending_counter_reads\":%u,\"accepted_a\":%u,\"accepted_b\":%u,\"phase_b_ms\":%u,\"session_accesses\":%u,"
        "\"period_cycles\":%u,\"after_failure\":%u,\"start\":[",max_pending,a,b,(unsigned)ms,total,FABRIC/20,
        after_failure);
    /* Observed sequences for the source comparison in checks/dram-controller.py. */
    for(unsigned i=0;i<6;++i)printf("%s[%u,%d]",i?",":"",(unsigned)(start_log[i]-PORT),start_written[i]);
    printf("],\"collect\":[");
    for(unsigned i=0;i<6;++i)printf("%s%u",i?",":"",(unsigned)(collect_log[i]-PORT));
    printf("],\"port\":%u}\n",(unsigned)PORT);
    puts("PASS latency window: start and collect sequences, pending cost, refusal rules, free-running "
         "fallback, failure latch, monotonic counter, phase spans, abandon, phase changes, copy-load stop "
         "and the evidence block");
    return 0;
}
