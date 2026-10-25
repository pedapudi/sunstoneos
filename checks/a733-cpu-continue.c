#include "a733_cpu_continue.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
struct model {
    struct a733_cpu_continue cpu;
    uint64_t now;
    uint32_t control,waker,enabled,pending,active,group,modifier,config,priority;
    unsigned accesses,calls,fail_at,late_at,stuck,bad_frame,bad_distributor,hotplug_writes;
    unsigned busy,wake_wait,sleep_wait,distributor_busy,distributor_stuck;
    uint32_t pwrr,implemented_group;
    unsigned power_writes,group_wait,group_stuck,power_stuck,reject_once;
    unsigned power_access[3];
};
static uintptr_t frame(unsigned core) {return 0x03460000+((core+3)%8)*0x20000;}
static int result(struct model *m)
{
    ++m->accesses;++m->calls;
    if(m->accesses==m->late_at)m->now=1000;
    return m->accesses!=m->fail_at;
}
static int clock_read(void *ctx,uint64_t *out)
{struct model *m=ctx;++m->calls;*out=m->now;return 1;}
static int read64(void *ctx,uintptr_t address,uint64_t *out)
{
    struct model *m=ctx;assert(address>=0x03460008 && address<=0x03540008);
    unsigned index=(unsigned)((address-0x03460008)/0x20000),core=(index+5)%8;
    *out=((uint64_t)core<<40)|(index==7?16:0);
    if(m->bad_frame && index==2)*out|=2;
    return result(m);
}
static int read32(void *ctx,uintptr_t address,uint32_t *out)
{
    struct model *m=ctx;uintptr_t c=0x07050200+m->cpu.core*4,r=frame(m->cpu.core),g=r+0x10000;
    if(address>=r && address<r+0x20000 && address!=r+0x24)assert(!(m->pwrr&1));
    if(address==0x03400000) {
        *out=(m->bad_distributor?0x73:0x33)|(m->distributor_busy?UINT32_C(0x80000000):0);
        if(m->distributor_busy && !m->distributor_stuck)--m->distributor_busy;
    }
    else if(address==c)*out=m->control;
    else if(address==c+0x20)*out=0x40;
    else if(address==r+0x24) {
        m->power_access[m->power_writes?2:0]=m->accesses+1;
        *out=m->pwrr;
        if(m->group_wait && !m->group_stuck && --m->group_wait==0)m->pwrr=13;
    }
    else if(address==r) {*out=m->busy?8:0;if(m->busy && !m->stuck)--m->busy;}
    else if(address==r+0x14) {
        if(m->sleep_wait && --m->sleep_wait==0)m->waker|=4;
        if(m->wake_wait && --m->wake_wait==0 && !m->stuck)m->waker&=~4u;
        *out=m->waker;
    }
    else if(address==g+0x100)*out=m->enabled;
    else if(address==g+0x300)*out=m->active;
    else if(address==g+0x80)*out=m->group;
    else if(address==g+0xd00)*out=m->modifier;
    else if(address==g+0xc04)*out=m->config;
    else if(address==g+0x400)*out=m->priority;
    else {assert(0);return 0;}
    return result(m);
}
static int write32(void *ctx,uintptr_t address,uint32_t value)
{
    struct model *m=ctx;uintptr_t c=0x07050200+m->cpu.core*4,r=frame(m->cpu.core),g=r+0x10000;
    if(address>=r && address<r+0x20000 && address!=r+0x24)assert(!(m->pwrr&1));
    if(address==c) {assert(m->cpu.core && value==(m->control&~2u));m->control=value;++m->hotplug_writes;}
    else if(address==r+0x24) {
        assert(value==0 && ((m->pwrr>>2)&1)==((m->pwrr>>3)&1));
        m->power_access[1]=m->accesses+1;++m->power_writes;
        if(m->reject_once)m->reject_once=0;
        else if(!m->power_stuck)m->pwrr=0;
    }
    else if(address==r+0x14) {
        assert(!(m->waker&2) || (m->waker&4));
        assert(value==(m->waker&~2u));m->waker=value;m->wake_wait=2;
    } else if(address==g+0x180) {m->enabled&=~value;m->busy=1;}
    else if(address==g+0x280)m->pending&=~value;
    else if(address==g+0x380)m->active&=~value;
    else if(address==g+0x80) {assert(!m->enabled);m->group=value&m->implemented_group;}
    else if(address==g+0xd00) {assert(!m->enabled);m->modifier=value;}
    else if(address==g+0xc04) {assert(!m->enabled);m->config=value;}
    else if(address==g+0x400) {assert(!m->enabled);m->priority=value;}
    else if(address==g+0x100) {
        assert(!m->pending && !m->active && m->group==m->implemented_group && !m->modifier);
        assert(!(m->waker&6));m->enabled|=value;m->busy=1;
    } else {assert(0);return 0;}
    return result(m);
}
static void setup(struct model *m,unsigned core)
{
    memset(m,0,sizeof(*m));m->now=1;m->control=0x42;m->waker=0x12;m->sleep_wait=2;
    m->pwrr=13;m->implemented_group=UINT32_MAX;
    m->enabled=UINT32_MAX;m->pending=UINT32_MAX;m->active=UINT32_MAX;
    m->modifier=UINT32_MAX;m->config=UINT32_MAX;m->priority=0x76543210;
    struct a733_cpu_continue_config c={.ops={read32,read64,write32,clock_read},.context=m,
        .epoch=42,.deadline_us=1000,.mpidr=0x81000000+(core<<8)};
    assert(a733_cpu_continue_init(&m->cpu,&c));memset(&c,0,sizeof(c));
}
static unsigned run(struct model *m)
{
    for(unsigned i=0;i<100 && m->cpu.state==A733_CONTINUE_PENDING;++i)a733_cpu_continue_poll(&m->cpu);
    return m->cpu.state;
}
static void quiet(struct model *m)
{unsigned n=m->calls;for(unsigned i=0;i<3;++i)a733_cpu_continue_poll(&m->cpu);assert(n==m->calls);}
static _Alignas(4096) uint64_t l1[512],l2[512];
static void power_cases(void)
{
    struct model m;unsigned positions[3];
    for(unsigned core=0;core<2;++core) {
        setup(&m,core);if(!core)m.pwrr=0;
        assert(run(&m)==A733_CONTINUE_GIC_READY && m.power_writes==1 && !m.pwrr);
        memcpy(positions,m.power_access,sizeof(positions));quiet(&m);
    }
    setup(&m,1);m.pwrr=5;m.group_wait=3;m.reject_once=1;
    assert(run(&m)==A733_CONTINUE_GIC_READY && m.power_writes==2 && !m.pwrr);quiet(&m);
    for(unsigned i=0;i<3;++i) {
        setup(&m,1);m.fail_at=positions[i];
        assert(run(&m)==A733_CONTINUE_RETAINED && m.cpu.error==1);quiet(&m);
    }
    setup(&m,1);m.pwrr=5;m.group_wait=1;m.group_stuck=1;
    assert(run(&m)==A733_CONTINUE_PENDING && !m.power_writes);
    m.now=1000;assert(run(&m)==A733_CONTINUE_RETAINED && m.cpu.error==2);quiet(&m);
    setup(&m,1);m.power_stuck=1;
    assert(run(&m)==A733_CONTINUE_PENDING && m.power_writes);
    m.now=1000;assert(run(&m)==A733_CONTINUE_RETAINED && m.cpu.error==2);quiet(&m);
    puts("PASS local GIC-600 power: primary/secondary, group transition and rejected request, three access failures, two deadline failures");
}
static void group_cases(void)
{
    struct model m;
    const uint32_t supported[]={UINT32_MAX,UINT32_C(0xfff0ffff),UINT32_C(0x6000ffff)};
    for(unsigned i=0;i<sizeof(supported)/sizeof(supported[0]);++i) {
        setup(&m,1);m.implemented_group=supported[i];
        assert(run(&m)==A733_CONTINUE_GIC_READY && m.group==supported[i]);quiet(&m);
    }
    for(unsigned bit=0;bit<32;++bit)if((UINT32_C(0x6000ffff)>>bit)&1) {
        setup(&m,1);m.implemented_group=UINT32_C(0xfff0ffff)&~(UINT32_C(1)<<bit);
        assert(run(&m)==A733_CONTINUE_RETAINED && m.cpu.error==4 && m.cpu.phase==23);quiet(&m);
    }
    puts("PASS three supported local group masks and refusal of each missing required SGI/timer bit");
}
int main(int argc,char **argv)
{
    if(argc==2 && !strcmp(argv[1],"--power")) {power_cases();return 0;}
    if(argc==2 && !strcmp(argv[1],"--groups")) {group_cases();return 0;}
    struct model m;unsigned operations=0;
    for(unsigned core=0;core<8;++core) {
        setup(&m,core);assert(run(&m)==A733_CONTINUE_GIC_READY);
        assert(m.cpu.redistributor==frame(core) && m.enabled==1 && !m.pending && !m.active);
        assert(m.hotplug_writes==!!core && m.control==(core?0x40u:0x42u));
        assert(m.priority==0x765432f0 && m.config==(UINT32_MAX&~((2u<<26)|(2u<<28))));
        if(core)operations=m.accesses;quiet(&m);
        uint64_t sgi=0;assert(a733_cpu_sgi1r(core,&sgi) && sgi==((uint64_t)core<<16|1));
    }
    for(unsigned n=1;n<=operations;++n) {
        setup(&m,4);m.fail_at=n;assert(run(&m)==A733_CONTINUE_RETAINED && m.cpu.error==1);quiet(&m);
        setup(&m,7);m.late_at=n;assert(run(&m)==A733_CONTINUE_RETAINED && m.cpu.error==2);quiet(&m);
    }
    setup(&m,5);m.bad_frame=1;assert(run(&m)==A733_CONTINUE_RETAINED && !m.cpu.writes_attempted);quiet(&m);
    setup(&m,5);m.bad_distributor=1;assert(run(&m)==A733_CONTINUE_RETAINED && !m.cpu.writes_attempted);
    setup(&m,5);m.control|=1;assert(run(&m)==A733_CONTINUE_RETAINED && !m.cpu.writes_attempted);
    /* GICD_CTLR.RWP: a write in progress is waited for; one that never
     * completes ends at the deadline with a time error in phase 0. */
    setup(&m,5);m.distributor_busy=3;assert(run(&m)==A733_CONTINUE_GIC_READY && !m.distributor_busy);quiet(&m);
    setup(&m,5);m.distributor_busy=1;m.distributor_stuck=1;assert(run(&m)==A733_CONTINUE_PENDING && !m.cpu.phase);
    m.now=1000;assert(run(&m)==A733_CONTINUE_RETAINED && m.cpu.error==2 && !m.cpu.phase && !m.cpu.writes_attempted);quiet(&m);
    setup(&m,5);m.bad_distributor=1;m.distributor_busy=1;assert(run(&m)==A733_CONTINUE_RETAINED && m.cpu.error==3);
    setup(&m,5);m.stuck=1;assert(run(&m)==A733_CONTINUE_PENDING);m.now=1000;
    assert(run(&m)==A733_CONTINUE_RETAINED && m.cpu.error==2);quiet(&m);
    setup(&m,5);assert(a733_cpu_continue_poll(&m.cpu)==A733_CONTINUE_PENDING);m.now=0;
    assert(run(&m)==A733_CONTINUE_RETAINED && m.cpu.error==2);
    uint64_t value=0xabc;assert(!a733_cpu_sgi1r(8,&value) && value==0xabc);
    assert(a733_cpu_continue_tables(l1,l2,0x42001000));
    assert((l1[0]&3)==1 && !(l1[0]&(3<<6)) && l1[1]==0x42001003);
    for(unsigned i=0;i<64;++i) {
        uint64_t d=l2[i];assert((d&0xffffffe00000ULL)==0x40000000+((uint64_t)i<<21));
        assert(((d>>2)&7)==1 && ((d>>8)&3)==3 && (d&(1<<10)));
        if(i==8)assert(((d>>6)&3)==3 && !(d&((1ULL<<53)|(1ULL<<54))));
        else {assert(d&(1ULL<<53));assert(d&(1ULL<<54));assert(((d>>6)&3)==(i==16?0u:1u));}
    }
    for(unsigned i=2;i<512;++i)assert(!l1[i]);
    for(unsigned i=64;i<512;++i)assert(!l2[i]);
    uint64_t old=l1[0];assert(!a733_cpu_continue_tables(l1,l1,0x42001000) && l1[0]==old);
    assert(!a733_cpu_continue_tables(l1,l2,0x41001000) && l1[0]==old);
    assert(!a733_cpu_continue_tables(l1+1,l2,0x42001000) && l1[0]==old);
    printf("PASS eight local GIC interfaces, %u uncertain/late operation positions, distributor write-pending wait and timeout, NC table permissions and Aff1 SGIs\n",operations);
}
