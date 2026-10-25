/* Generalize the physically demonstrated core-1 local initialization/cleanup.
 * The linked a733_cpu_continue functions come from the frozen production ELF. */
#include "secondary-local-probe.h"
#include "a733_cpu_continue.h"
#ifdef SUNSTONE_SECONDARY_CACHE
extern int native_el3_cache_probe(uint64_t epoch,uint64_t deadline);
#endif

struct context { unsigned core;uintptr_t base;volatile struct secondary_local_record *report; };
static void barrier(void) {__asm__ volatile("dsb sy":::"memory");}
static uint64_t ticks(void)
{uint64_t n;__asm__ volatile("isb\n mrs %0,cntpct_el0":"=r"(n)::"memory");return n;}
static uint32_t load(struct context *c,uintptr_t address)
{
    volatile struct secondary_local_access *a=&c->report->access;
    a->completed=0;a->address=address;a->operation=1;barrier();
    uint32_t v=*(volatile uint32_t *)address;barrier();
    a->value=v;a->completed=1;barrier();return v;
}
static void store(struct context *c,uintptr_t address,uint32_t value)
{
    volatile struct secondary_local_access *a=&c->report->access;
    a->completed=0;a->address=address;a->operation=3;a->value=value;barrier();
    *(volatile uint32_t *)address=value;barrier();a->completed=1;barrier();
}
static int read64(void *context,uintptr_t address,uint64_t *out)
{
    struct context *c=context;
    if(address<0x03460008 || address>0x03540008 || ((address-0x03460008)&0x1ffff))return 0;
    volatile struct secondary_local_access *a=&c->report->access;
    a->completed=0;a->address=address;a->operation=2;barrier();
    *out=*(volatile uint64_t *)address;barrier();a->value=*out;a->completed=1;barrier();return 1;
}
static int read32(void *context,uintptr_t address,uint32_t *out)
{
    struct context *c=context;uintptr_t r=c->base,g=r+0x10000,control=0x07050200+c->core*4;
    if(address!=0x03400000 && address!=control && address!=control+0x20 &&
       address!=r && address!=r+0x14 && address!=r+0x24 && address!=g+0x300 &&
       address!=g+0x80 && address!=g+0xd00 && address!=g+0xc04 && address!=g+0x400 && address!=g+0x100)return 0;
    *out=load(c,address);return 1;
}
static int write32(void *context,uintptr_t address,uint32_t value)
{
    struct context *c=context;uintptr_t r=c->base,g=r+0x10000,control=0x07050200+c->core*4;
    if(address!=control && address!=r+0x14 && address!=r+0x24 &&
       address!=g+0x180 && address!=g+0x280 && address!=g+0x380 && address!=g+0x80 &&
       address!=g+0xd00 && address!=g+0xc04 && address!=g+0x400 && address!=g+0x100)return 0;
    store(c,address,value);return 1;
}
static int now_us(void *context,uint64_t *out) {(void)context;*out=ticks()/24;return 1;}
static __attribute__((noinline)) int wait_bits(struct context *c,uintptr_t address,uint32_t mask,uint32_t expected)
{
    uint64_t previous=ticks();
    do {
        uint64_t now=ticks();if(now<previous || now>=c->report->deadline)return 0;
        previous=now;if((load(c,address)&mask)==expected)return 1;
    }while(1);
}
static void snapshot(struct context *c,volatile struct secondary_local_state *s)
{
    uintptr_t r=c->base,g=r+0x10000;
    s->ctlr=load(c,r);s->waker=load(c,r+0x14);
    s->enable=load(c,g+0x100);s->pending=load(c,g+0x200);s->active=load(c,g+0x300);
    s->group=load(c,g+0x80);s->modifier=load(c,g+0xd00);
    s->configuration=load(c,g+0xc04);s->priority=load(c,g+0x400);s->pwrr=load(c,r+0x24);
}
/* GICR_CTLR.UWP (bit 31) and RWP (bit 3) are read-only status (IHI 0069G
 * 12.11.2). The board has read RWP set after a completed wait with no write
 * by this core in between, for a reason not yet explained. Linux and TF-A
 * poll these bits and never compare GICR_CTLR, so comparisons and baselines
 * ignore them. */
#define CTLR_STATUS UINT64_C(0x80000008)
static int same(const volatile struct secondary_local_state *a,const volatile struct secondary_local_state *b)
{
    const volatile uint64_t *x=(const volatile uint64_t *)a,*y=(const volatile uint64_t *)b;
    for(unsigned i=0;i<sizeof(*a)/8;++i)if((x[i]^y[i])&(i?UINT64_MAX:~CTLR_STATUS))return 0;
    return 1;
}
static int cleanup(struct context *c)
{
    volatile struct secondary_local_record *r=c->report;
    const volatile struct secondary_local_state *b=&r->before;
    uintptr_t base=c->base,g=base+0x10000;
    uint64_t g0,g1;
    __asm__ volatile("mrs %0,icc_igrpen0_el1\n mrs %1,icc_igrpen1_el3":"=r"(g0),"=r"(g1));
    if((g0&1) || (g1&3)) {r->cleanup_error=1;return 0;}
    ++r->cleanup_writes;store(c,g+0x180,UINT32_MAX);
    if(!wait_bits(c,base,8,0)) {r->cleanup_error=2;return 0;}
    if(load(c,g+0x100) || load(c,g+0x200) || load(c,g+0x300)) {r->cleanup_error=3;return 0;}
    const uintptr_t addresses[]={g+0x80,g+0xd00,g+0xc04,g+0x400};
    const uint32_t values[]={(uint32_t)b->group,(uint32_t)b->modifier,
                            (uint32_t)b->configuration,(uint32_t)b->priority};
    for(unsigned i=0;i<4;++i) {++r->cleanup_writes;store(c,addresses[i],values[i]);}
    /* GICR_CTLR.RWP is read-only status and part of the final comparison.
     * Wait for it after the restoring writes, as Linux gic_cpu_init does. */
    if(!wait_bits(c,base,8,0)) {r->cleanup_error=7;return 0;}
    /* The final snapshot comparison (error 6) covers these four words; the
     * separate readback (former error 4) was removed to fit the overlay. */
    ++r->cleanup_writes;store(c,base+0x14,(uint32_t)b->waker);
    if(!wait_bits(c,base+0x14,4,4)) {r->cleanup_error=5;return 0;}
    ++r->cleanup_writes;store(c,0x07050200+c->core*4,(uint32_t)r->control_before);
    r->control_after=load(c,0x07050200+c->core*4);snapshot(c,&r->after);
    if(r->control_after!=r->control_before || !same(&r->before,&r->after)) {r->cleanup_error=6;return 0;}
    return 1;
}
int secondary_local_probe(unsigned core,uint64_t epoch,uint64_t deadline,
                          volatile struct secondary_local_record *r)
{
    uint64_t mpidr,frequency,el,daif;unsigned actual;
    __asm__ volatile("mrs %0,mpidr_el1\n mrs %1,cntfrq_el0\n mrs %2,CurrentEL\n mrs %3,daif"
        :"=r"(mpidr),"=r"(frequency),"=r"(el),"=r"(daif));
    if(!r || !core || core>=8 || !epoch || frequency!=24000000 || el!=12 ||
       (daif&0x3c0)!=0x3c0 || !a733_cpu_index(mpidr,&actual) || actual!=core || ticks()>=deadline)return 0;
    const volatile uint64_t *words=(const volatile uint64_t *)r;
    for(unsigned i=0;i<sizeof(*r)/8;++i)if(words[i])return 0;
    r->state=1;r->epoch=epoch;r->core=core;r->deadline=deadline;
    // Recorded A733 frame order; verify the selected affinity before any local write.
    struct context c={core,0x03460000+core*0x20000,r};r->base=c.base;
    uint64_t typer;if(!read64(&c,c.base+8,&typer) || (typer>>32)!=(uint64_t)core<<8 ||
       (typer&2) || (!!(typer&16)!=(core==7)))return 0;
    uint32_t power=load(&c,c.base+0x24);
    if((power&1) || (((power>>2)^(power>>3))&1))return 0;
    snapshot(&c,&r->before);
    const volatile struct secondary_local_state *b=&r->before;
    if((b->ctlr&~CTLR_STATUS) || b->waker!=6 || b->enable || b->pending || b->active ||
       b->pwrr!=power || ticks()>=deadline)return 0;
    r->control_before=load(&c,0x07050200+core*4);
    if((r->control_before|load(&c,0x07050220+core*4))&1)return 0;
    uint64_t sre,g0,g1;
    __asm__ volatile("mrs %0,icc_sre_el3":"=r"(sre));r->sre_before=sre;sre|=7;
    __asm__ volatile("msr icc_sre_el3,%0\n isb"::"r"(sre):"memory");
    __asm__ volatile("mrs %0,icc_sre_el3":"=r"(sre));r->sre_after=sre;
    if((sre&7)!=7)return 0;
    __asm__ volatile("mrs %0,icc_igrpen0_el1\n mrs %1,icc_igrpen1_el3":"=r"(g0),"=r"(g1));
    r->group0=g0;r->group1=g1;if((g0&1) || (g1&3))return 0;
    struct a733_cpu_continue state={0};
    struct a733_cpu_continue_config config={.ops={read32,read64,write32,now_us},.context=&c,
        .epoch=epoch,.deadline_us=deadline/24,.mpidr=mpidr};
    if(!a733_cpu_continue_init(&state,&config))return 0;
    while(a733_cpu_continue_poll(&state)==A733_CONTINUE_PENDING) {}
    r->controller_access=r->access;
    r->controller_state=state.state;r->phase=state.phase;r->error=state.error;
    r->writes=state.writes_attempted;if(state.redistributor!=c.base)return 0;
    if(state.state!=A733_CONTINUE_GIC_READY &&
       !(state.state==A733_CONTINUE_RETAINED && state.phase>=10 && state.phase<=26 &&
         state.writes_attempted>=4 && ticks()<deadline))return 0;
    r->state=2;
#ifdef SUNSTONE_SECONDARY_CACHE
    if(state.state==A733_CONTINUE_GIC_READY && !native_el3_cache_probe(epoch,deadline))return 0;
#endif
    if(!cleanup(&c))return 0;
    barrier();r->state=state.state==A733_CONTINUE_GIC_READY?3:4;barrier();
    return state.state==A733_CONTINUE_GIC_READY?1:2;
}
