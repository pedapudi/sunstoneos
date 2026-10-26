#include "a733_cpu_start.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#ifdef SUNSTONE_BOARD_RESIDENT
#define NREG 7
#define NWRITE 8
#define INITIAL_STATUS 0
#define ON_STATUS 0x108
#else
#define NREG 6
#define NWRITE 7
#define INITIAL_STATUS 0x12340000
#define ON_STATUS 0x12340008
#endif
struct model {
    uint32_t regs[NREG];unsigned core,ops,clocks,fail_at,late_at,corrupt_at,hold,waits,writes,published,immediate,dynamic_hold;
    uint64_t now;
    struct {uintptr_t address;uint32_t value;} trace[9];
};
static uintptr_t address(const struct model *m,unsigned n)
{
    uintptr_t b=UINT32_C(0x08001000)+m->core*0x1000u;
    const uintptr_t a[]={b,b+4,b+8,UINT32_C(0x07050200)+m->core*4u,
        UINT32_C(0x07050220)+m->core*4u,UINT32_C(0x07052008)+m->core*0x1000u
#ifdef SUNSTONE_BOARD_RESIDENT
        ,UINT32_C(0x07052000)+m->core*0x1000u
#endif
    };
    return a[n];
}
static unsigned reg(struct model *m,uintptr_t a)
{for(unsigned i=0;i<NREG;++i)if(a==address(m,i))return i;assert(0);return 0;}
static int action(struct model *m)
{++m->ops;if(m->ops==m->late_at)m->now=1000;return m->ops!=m->fail_at;}
static int read32(void *ctx,uintptr_t a,uint32_t *out)
{
    struct model *m=ctx;unsigned r=reg(m,a);
    if(r==5 && (m->regs[3]&1) && !m->hold && ++m->waits>=2)m->regs[5]=ON_STATUS;
#ifdef SUNSTONE_BOARD_RESIDENT
    if(r==5 && m->regs[6]==0x100 && !m->dynamic_hold)m->regs[5]|=0x100;
#endif
    *out=m->regs[r];if(m->ops+1==m->corrupt_at)*out^=1;return action(m);
}
static int write32(void *ctx,uintptr_t a,uint32_t v)
{
    struct model *m=ctx;unsigned r=reg(m,a);assert(r!=5 && m->writes<9);
    m->trace[m->writes].address=a;m->trace[m->writes++].value=v;m->regs[r]=v;
#ifdef SUNSTONE_BOARD_RESIDENT
    if(r==6) {
        assert(m->published && (m->regs[0]&1) && m->regs[1]==0x41000000+m->core*0x4000 && !m->regs[2]);
        if(m->immediate)m->regs[5]=ON_STATUS;
    }
#endif
    return action(m); /* An uncertain write may already have taken effect. */
}
static int barrier(void *ctx) {struct model *m=ctx;m->published=1;return action(ctx);}
static int now_us(void *ctx,uint64_t *out)
{struct model *m=ctx;++m->clocks;*out=m->now++;return 1;}
static struct a733_cpu_start_config config(struct model *m,unsigned core)
{
    *m=(struct model){.core=core,.now=1,.regs={0xa0,0,0,0x42,0x80,INITIAL_STATUS}};
    return (struct a733_cpu_start_config){.ops={read32,write32,barrier,now_us},.context=m,
        .boot_mpidr=0x81000000,.entry=0x41000000+core*0x4000,.lease=0x12345678,
        .deadline=1000,.core=core};
}
static void run(struct a733_cpu_start *s,struct model *m)
{
    for(unsigned n=0;n<100 && s->state==A733_CPU_START_PENDING;++n) {
        unsigned old=m->ops;a733_cpu_start_poll(s);assert(m->ops-old<=1);
    }
    assert(s->state!=A733_CPU_START_PENDING);
}
static void quiet(struct a733_cpu_start *s,struct model *m)
{
    struct model old=*m;struct a733_cpu_start saved=*s;
    assert(s->config.lease==0x12345678);
    for(unsigned i=0;i<3;++i)(void)a733_cpu_start_poll(s);
    assert(!memcmp(&old,m,sizeof(old)) && !memcmp(&saved,s,sizeof(saved)));
    assert(!a733_cpu_start_init(s,&s->config));assert(!memcmp(&saved,s,sizeof(saved)));
}
int main(void)
{
    struct model m;unsigned actions=0;
    for(unsigned core=1;core<A733_CPU_COUNT;++core) {
        struct a733_cpu_start s={0};struct a733_cpu_start_config c=config(&m,core);
        assert(a733_cpu_start_init(&s,&c));memset(&c,0,sizeof(c));assert(!m.ops&&!m.clocks);
        run(&s,&m);assert(s.state==A733_CPU_START_POWERED && m.regs[3]==0x42 && m.regs[4]==0x80);
        assert(s.writes_attempted==NWRITE && m.writes==NWRITE && (m.regs[5]&15)==8);actions=m.ops;
        for(unsigned i=0;i<m.writes;++i)printf("TRACE %u %lx %x\n",core,(unsigned long)m.trace[i].address,m.trace[i].value);
        quiet(&s,&m);
    }
    for(unsigned i=1;i<=actions;++i) {
        struct a733_cpu_start s={0};struct a733_cpu_start_config c=config(&m,1);m.fail_at=i;
        assert(a733_cpu_start_init(&s,&c));run(&s,&m);assert(s.state==A733_CPU_START_RETAINED && s.error==A733_CPU_START_IO);quiet(&s,&m);
    }
    for(unsigned i=1;i<=actions;++i) {
        struct a733_cpu_start s={0};struct a733_cpu_start_config c=config(&m,7);m.late_at=i;
        assert(a733_cpu_start_init(&s,&c));run(&s,&m);assert(s.state==A733_CPU_START_RETAINED && s.error==A733_CPU_START_TIME);quiet(&s,&m);
    }
    for(unsigned r=3;r<6;++r) {
        struct a733_cpu_start s={0};struct a733_cpu_start_config c=config(&m,2);m.regs[r]|=1;
        assert(a733_cpu_start_init(&s,&c));run(&s,&m);assert(s.error==A733_CPU_START_INITIAL_STATE && !m.writes);quiet(&s,&m);
    }
    const unsigned verify[]={
#ifdef SUNSTONE_BOARD_RESIDENT
        9,10,12,15,23,24
#else
        7,8,10,18,19
#endif
    };
    for(unsigned n=0;n<sizeof(verify)/sizeof(*verify);++n) {
        struct a733_cpu_start s={0};struct a733_cpu_start_config c=config(&m,3);m.corrupt_at=verify[n];
        assert(a733_cpu_start_init(&s,&c));run(&s,&m);assert(s.error==A733_CPU_START_VERIFY);quiet(&s,&m);
    }
    {
        struct a733_cpu_start s={0};struct a733_cpu_start_config c=config(&m,1);m.hold=1;c.deadline=80;
        assert(a733_cpu_start_init(&s,&c));run(&s,&m);assert(s.error==A733_CPU_START_TIME);
        assert((m.regs[3]&1) && (m.regs[4]&1));quiet(&s,&m);
    }
    {
        struct a733_cpu_start s={0};struct a733_cpu_start_config c=config(&m,1);
        assert(a733_cpu_start_init(&s,&c));a733_cpu_start_poll(&s);m.now=0;
        a733_cpu_start_poll(&s);assert(s.error==A733_CPU_START_TIME);quiet(&s,&m);
    }
#ifdef SUNSTONE_BOARD_RESIDENT
    for(unsigned immediate=0;immediate<2;++immediate) {
        struct a733_cpu_start s={0};struct a733_cpu_start_config c=config(&m,6);
        m.immediate=immediate;m.dynamic_hold=!immediate;c.deadline=80;
        assert(a733_cpu_start_init(&s,&c));run(&s,&m);
        assert(immediate?s.state==A733_CPU_START_POWERED:s.error==A733_CPU_START_TIME);
        assert(m.regs[6]==0x100 && (m.regs[3]&2));quiet(&s,&m);
    }
    for(unsigned bad=0;bad<2;++bad) {
        struct a733_cpu_start s={0};struct a733_cpu_start_config c=config(&m,1);
        if(bad)m.regs[6]=0x100;else m.regs[3]&=~2u;
        assert(a733_cpu_start_init(&s,&c));run(&s,&m);
        assert(s.error==A733_CPU_START_INITIAL_STATE && !m.writes && m.regs[6]==(bad?0x100u:0u));quiet(&s,&m);
    }
#endif
    for(unsigned core=0;core<8;++core) {
        unsigned out=99;assert(a733_cpu_index((uint64_t)core<<8,&out)&&out==core);
        assert(a733_cpu_index(UINT64_C(0x81000000)|((uint64_t)core<<8),&out)&&out==core);
    }
    const uint64_t bad[]={1,0x800,0x10000,UINT64_C(1)<<32,UINT64_C(1)<<40,UINT64_C(1)<<25};
    for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);++i) {unsigned out=99;assert(!a733_cpu_index(bad[i],&out)&&out==99);}
    for(unsigned n=0;n<7;++n) {
        struct a733_cpu_start s={0},saved=s;struct a733_cpu_start_config c=config(&m,1);
        switch(n) {case 0:c.core=0;break;case 1:c.core=8;break;case 2:c.entry=3;break;
        case 3:c.entry=UINT64_C(1)<<32;break;case 4:c.lease=0;break;case 5:c.boot_mpidr=0x81000100;break;case 6:c.deadline=0;break;}
        assert(!a733_cpu_start_init(&s,&c));assert(!memcmp(&s,&saved,sizeof(s))&&!m.ops&&!m.clocks);
    }
    puts("PASS CPU power-on: seven secondary targets, bounded steps, retained failures/deadlines, topology and admission");
}
