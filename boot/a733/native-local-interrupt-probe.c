/* Returning core-1 diagnostic. Runs the production portable continuation. */
#include "a733_cpu_continue.h"

struct baseline {
    uint64_t state, base, ctlr, waker, enable, pending, active;
    uint64_t group, modifier, config, priority, pwrr;
};
struct result {
    uint64_t state, epoch, core, deadline, controller_state, phase, error, writes;
    uint64_t redistributor, original_control, restored_control, sre_before, sre_after;
    uint64_t igrpen0, igrpen1, waker, enable, pending, active, group, modifier;
    uint64_t config, priority, pwrr, cleanup_error, cleanup_writes, stopped_before_power, reserved[5];
};
_Static_assert(sizeof(struct result)==256,"Host result layout");
static void barrier(void) {__asm__ volatile("dsb sy":::"memory");}
static uint32_t load(uintptr_t address)
{barrier();uint32_t v=*(volatile uint32_t *)address;barrier();return v;}
static void store(uintptr_t address,uint32_t value)
{barrier();*(volatile uint32_t *)address=value;barrier();}
static int read32(void *context,uintptr_t a,uint32_t *out)
{
    (void)context;
    if(a!=0x03400000 && a!=0x07050204 && a!=0x07050224 &&
       a!=0x03480000 && a!=0x03480014 && a!=0x03480024 &&
       a!=0x03490300 && a!=0x03490080 && a!=0x03490d00 &&
       a!=0x03490c04 && a!=0x03490400 && a!=0x03490100)return 0;
    *out=load(a);return 1;
}
static int read64(void *context,uintptr_t a,uint64_t *out)
{
    (void)context;
    if(a<0x03460008 || a>0x03540008 || ((a-0x03460008)&0x1ffff))return 0;
    barrier();*out=*(volatile uint64_t *)a;barrier();return 1;
}
static int write32(void *context,uintptr_t a,uint32_t v)
{
    (void)context;
#if defined(SUNSTONE_PROBE_STOP_BEFORE_POWER)
    if(a==0x03480024) {
        volatile struct result *r=(void *)UINT32_C(0x42002980);
        r->stopped_before_power=1;
        return 0; /* Deliberate callback refusal; no redistributor write occurs. */
    }
#endif
    if(a!=0x07050204 && a!=0x03480014 && a!=0x03480024 &&
       a!=0x03490180 && a!=0x03490280 && a!=0x03490380 &&
       a!=0x03490080 && a!=0x03490d00 && a!=0x03490c04 &&
       a!=0x03490400 && a!=0x03490100)return 0;
    store(a,v);return 1;
}
static uint64_t ticks(void)
{uint64_t v;__asm__ volatile("isb\n mrs %0,cntpct_el0":"=r"(v)::"memory");return v;}
static int now_us(void *context,uint64_t *out)
{(void)context;*out=ticks()/24;return 1;}
static int wait_clear(uintptr_t address,uint32_t mask,uint64_t deadline)
{
    uint64_t previous=ticks();
    for(;;) {
        uint64_t now=ticks();
        if(now<previous || now>=deadline)return 0;
        previous=now;
        if(!(load(address)&mask))return 1;
    }
}
static int cleanup(volatile struct result *r,const volatile struct baseline *b)
{
    /* Group delivery was never enabled. Keep it disabled before ProcessorSleep. */
    uint64_t g0,g1;
    __asm__ volatile("mrs %0,icc_igrpen0_el1\n mrs %1,icc_igrpen1_el3":"=r"(g0),"=r"(g1));
    if((g0&1) || (g1&3)) {r->cleanup_error=1;return 0;}
    ++r->cleanup_writes;store(0x03490180,UINT32_MAX);
    if(!wait_clear(0x03480000,8,r->deadline)) {r->cleanup_error=2;return 0;}
    r->enable=load(0x03490100);r->pending=load(0x03490200);r->active=load(0x03490300);
    if(r->enable || r->pending || r->active) {r->cleanup_error=3;return 0;}
    const uintptr_t addresses[]={0x03490080,0x03490d00,0x03490c04,0x03490400};
    const uint32_t values[]={(uint32_t)b->group,(uint32_t)b->modifier,(uint32_t)b->config,(uint32_t)b->priority};
    for(unsigned i=0;i<4;++i) {++r->cleanup_writes;store(addresses[i],values[i]);}
    r->group=load(addresses[0]);r->modifier=load(addresses[1]);
    r->config=load(addresses[2]);r->priority=load(addresses[3]);
    if(r->group!=b->group || r->modifier!=b->modifier || r->config!=b->config || r->priority!=b->priority) {
        r->cleanup_error=4;return 0;
    }
    /* The power request leaves this initially powered frame powered. */
    ++r->cleanup_writes;store(0x03480014,(uint32_t)b->waker);
    uint64_t previous=ticks();
    do {
        uint64_t now=ticks();
        if(now<previous || now>=r->deadline) {r->cleanup_error=5;return 0;}
        previous=now;r->waker=load(0x03480014);
    } while(!(r->waker&4));
    r->pwrr=load(0x03480024);
    ++r->cleanup_writes;store(0x07050204,(uint32_t)r->original_control);
    r->restored_control=load(0x07050204);
    if(r->waker!=b->waker || r->pwrr!=b->pwrr || r->restored_control!=r->original_control || load(0x03480000)!=b->ctlr) {
        r->cleanup_error=6;return 0;
    }
    return 1;
}

int native_local_interrupt_probe(uint64_t epoch,uint64_t deadline)
{
    volatile struct result *r=(void *)UINT32_C(0x42002980);
    const volatile struct baseline *b=(void *)UINT32_C(0x42002900);
    uint64_t mpidr,frequency,el;
    __asm__ volatile("mrs %0,mpidr_el1\n mrs %1,cntfrq_el0\n mrs %2,CurrentEL":"=r"(mpidr),"=r"(frequency),"=r"(el));
    unsigned core;
    if(r->state || !epoch || frequency!=24000000 || el!=12 ||
       !a733_cpu_index(mpidr,&core) || core!=1)return 0;
    r->epoch=epoch;r->core=core;r->deadline=deadline;r->state=1;
    if(b->state!=2 || b->base!=0x03480000 || b->ctlr || b->waker!=6 ||
       b->enable || b->pending || b->active || (b->pwrr&1) ||
       (((b->pwrr>>2)^(b->pwrr>>3))&1) || ticks()>=deadline) {
        r->state=4;return 2;
    }
    r->original_control=load(0x07050204);
    if((r->original_control|load(0x07050224))&1) {r->state=4;return 2;}
    uint64_t sre,g0,g1;
    __asm__ volatile("mrs %0,icc_sre_el3":"=r"(sre));r->sre_before=sre;
    sre|=7; /* EL3 system registers and disabled legacy bypass. */
    __asm__ volatile("msr icc_sre_el3,%0\n isb"::"r"(sre):"memory");
    __asm__ volatile("mrs %0,icc_sre_el3":"=r"(sre));r->sre_after=sre;
    if((sre&7)!=7)return 0;
    __asm__ volatile("mrs %0,icc_igrpen0_el1\n mrs %1,icc_igrpen1_el3":"=r"(g0),"=r"(g1));
    r->igrpen0=g0;r->igrpen1=g1;
    if((g0&1) || (g1&3))return 0;
    struct a733_cpu_continue state={0};
    struct a733_cpu_continue_config config={.ops={read32,read64,write32,now_us},
        .context=&state,.epoch=epoch,.deadline_us=deadline/24,.mpidr=mpidr};
    if(!a733_cpu_continue_init(&state,&config))return 0;
    while(a733_cpu_continue_poll(&state)==A733_CONTINUE_PENDING) {}
    r->controller_state=state.state;r->phase=state.phase;r->error=state.error;
    r->writes=state.writes_attempted;r->redistributor=state.redistributor;
#if defined(SUNSTONE_PROBE_STOP_BEFORE_POWER)
    if(r->stopped_before_power && state.state==A733_CONTINUE_RETAINED &&
       state.redistributor==b->base && state.writes_attempted==2) {
        /* Only the core wake-control write ran. Verify the untouched local bank
         * and restore that control before the existing target shutdown. */
        r->pwrr=load(0x03480024);
        if(r->pwrr!=b->pwrr)return 0;
        r->waker=load(0x03480014);r->enable=load(0x03490100);
        r->pending=load(0x03490200);r->active=load(0x03490300);
        r->group=load(0x03490080);r->modifier=load(0x03490d00);
        r->config=load(0x03490c04);r->priority=load(0x03490400);
        if(load(0x03480000)!=b->ctlr || r->waker!=b->waker || r->enable!=b->enable ||
           r->pending!=b->pending || r->active!=b->active || r->group!=b->group ||
           r->modifier!=b->modifier || r->config!=b->config || r->priority!=b->priority)return 0;
        ++r->cleanup_writes;store(0x07050204,(uint32_t)r->original_control);
        r->restored_control=load(0x07050204);
        if(r->restored_control!=r->original_control)return 0;
        barrier();r->state=4;barrier();return 2;
    }
#endif
    if(state.state!=A733_CONTINUE_GIC_READY || state.redistributor!=b->base)return 0;
    r->state=2;
    if(!cleanup(r,b))return 0;
    barrier();r->state=3;barrier();return 1;
}
