#include "a733_npu_startup.h"
#include "a733_clock_reference.h"
#include "a733_npu_platform_native.h"
#include "a733_range.h"

#define READY A733_NPU_PLATFORM_READY
#define PENDING A733_NPU_PLATFORM_PENDING
#define RETAINED A733_NPU_PLATFORM_RETAINED
#define CCU UINT32_C(0x02002000)
#define PCK A733_POWER_PCK_DOMAIN(4)
#define BIT(n) (UINT32_C(1)<<(n))
#define TICK_US (1000000u/configTICK_RATE_HZ)
#define SETTLE_US 2000u
/* The supply wait ends when the owner sets its done bit. The owner bounds that
 * by its settle time after its first step, or by the PMIC provider's startup
 * deadline, plus its verification budget (docs/power-domains.md). */
#define SUPPLY_FAULTS (A733_POWER_PMIC_TRANSFER|A733_POWER_DOMAIN4_FAULT)

#if defined(__aarch64__) && !defined(__linux__)
static int privileged(void)
{uint64_t level;__asm__ volatile("mrs %0,CurrentEL":"=r"(level));return level==4;}
#else
/* Host models run the sequencer as one EL1 task. */
static int privileged(void){return 1;}
#endif

/* Native callbacks: the NPU allowlist adapter, the counter clock, the raw
 * counter and the shared register lock. */
static int native_read(void *v,uintptr_t a,uint32_t *out){(void)v;return a733_npu_platform_native_read(0,a,out);}
static int native_write(void *v,uintptr_t a,uint32_t value){(void)v;return a733_npu_platform_native_write(0,a,value);}
static int native_now(void *v,uint64_t *out)
{
    struct a733_npu_startup *s=v;
    return !atomic_load_explicit(&s->providers.locks->retained,memory_order_acquire) &&
        a733_counter_clock_now(s->providers.counter,out)==1;
}
/* Section 1.7: the LOCK spacing reads CNTPCT_EL0 directly; the counter clock
 * verified its 24 MHz frequency at initialization. */
static uint64_t native_spin_us(void *v)
{
    (void)v;uint64_t t=0;
#if defined(__aarch64__) && !defined(__linux__)
    __asm__ volatile("isb\n mrs %0,cntpct_el0":"=r"(t));
#endif
    return t/24;
}
static int native_enter(void *v,uint64_t deadline)
{struct a733_npu_startup *s=v;return a733_platform_lock_enter(s->providers.locks,A733_PLATFORM_REGISTER_LOCK,deadline);}
static void native_leave(void *v)
{struct a733_npu_startup *s=v;a733_platform_lock_leave(s->providers.locks,A733_PLATFORM_REGISTER_LOCK);}
static void native_retain(void *v)
{struct a733_npu_startup *s=v;(void)a733_platform_lock_retain(s->providers.locks,A733_PLATFORM_REGISTER_LOCK);}

static int counted_read(void *v,uintptr_t a,uint32_t *out)
{struct a733_npu_startup *s=v;++s->reads;return s->ops.registers.read(s->cookie,a,out);}
static int counted_write(void *v,uintptr_t a,uint32_t value)
{struct a733_npu_startup *s=v;++s->writes;return s->ops.registers.write(s->cookie,a,value);}
static const struct a733_npu_register_ops counted={counted_read,counted_write};

static int now(struct a733_npu_startup *s,uint64_t *out)
{
    uint64_t t;++s->clock_reads;
    if(s->ops.now(s->cookie,&t)!=1 || !a733_deadline_admit(t,&s->last_now,s->config.deadline_us))return 0;
    if(out)*out=t;
    return 1;
}
static int rd(struct a733_npu_startup *s,uintptr_t a,uint32_t *v){return counted_read(s,a,v)==1;}
static int check(struct a733_npu_startup *s,uintptr_t a,uint32_t mask,uint32_t expected)
{uint32_t v;return rd(s,a,&v) && (v&mask)==expected;}
static enum a733_npu_platform_result fault(struct a733_npu_startup *s)
{
    s->failed=1;
    if(s->register_held)s->ops.retain(s->cookie);
    return RETAINED;
}
/* Before any NPU write: release every token and stop. */
static enum a733_npu_platform_result refuse(struct a733_npu_startup *s)
{
    const uint64_t deadline=s->config.deadline_us;
    if(s->pck.token)(void)a733_gpu_resources_release(s->providers.pck,s->config.pck_client,s->pck.token,deadline);
    if(s->rail.id)(void)a733_rail_release(s->providers.rails,&s->rail,deadline);
    if(s->clock.id)(void)a733_clock_release(s->providers.clocks,&s->clock,deadline);
    s->failed=1;return RETAINED;
}
static int enter(struct a733_npu_startup *s)
{
    ++s->lock_enters;
    int held=s->ops.enter(s->cookie,s->config.deadline_us);
    if(held==1)s->register_held=1;
    return held;
}
static int leave(struct a733_npu_startup *s)
{
    s->ops.leave(s->cookie);s->register_held=0;
    return now(s,0);
}

static const uint32_t fabric_offsets[]={0x500,0x580,0x588};
static const uint32_t fabric_masks[]={0x0300001f,0x8700001f,0x8700001f};
static const uint32_t parent_mhz[3][8]={
    {24,0,0,600,0,0,0,0}, {24,0,800,600,480,0,0,0}, {24,600,0,480,400,0,0,0}
};
/* PERI0 outputs that a fabric clock selects: the PLL is enabled, locked and
 * unmodulated, and each selected output has its No-Auto route, software
 * enable and effective gate (A733 User Manual §4.1.4.6, §4.1.6.255, 264). */
static int peri0(struct a733_npu_startup *s)
{
    uint32_t pll,a,b,automatic,status;
    if(!rd(s,CCU+0xa0,&pll) || (pll&UINT32_C(0xfe000000))!=UINT32_C(0xfe000000) || (pll&BIT(1)) ||
       !rd(s,CCU+0xa8,&a) || (a&BIT(31)) || !rd(s,CCU+0xac,&b) || (b&BIT(27)) ||
       !rd(s,CCU+0x1908,&automatic) || !rd(s,CCU+0x1988,&status))return 0;
    unsigned n=((pll>>8)&255)+1;
    if(n<53 || n>105)return 0;
    for(unsigned i=0;i<3;++i) {
        unsigned mhz=parent_mhz[i][(s->config.fabric.fabric[i]>>24)&7],divisor,mask;
        switch(mhz) {
        case 24:continue;
        case 600:divisor=(((pll>>20)&7)+1)*2;mask=BIT(11)|BIT(9);break;
        case 400:divisor=(((pll>>20)&7)+1)*3;mask=BIT(11)|BIT(2)|BIT(1);break;
        case 800:divisor=((pll>>16)&7)+1;mask=BIT(10);break;
        case 480:divisor=((pll>>2)&7)+1;mask=BIT(8)|BIT(7);break;
        default:return 0;
        }
        unsigned gates=mask<<16;
        if((automatic&(mask|gates))!=(mask|gates) || (status&gates)!=gates || 24u*n!=mhz*divisor)return 0;
    }
    return 1;
}
/* The register pass before the first write (npu-platform.md): the reference
 * clock, the fabric words against the record, PERI0 when selected, every
 * PLL_NPU consumer under its mask, PLL_NPU disabled with its tuning words at
 * reset, and domain 4 OFF. */
static int pass(struct a733_npu_startup *s)
{
    uint32_t osc,ref,v;
    if(!rd(s,0x07090160,&osc) || !rd(s,CCU,&ref) || !a733_clock_ref24_valid(osc,ref))return 0;
    for(unsigned i=0;i<3;++i) {
        if(!rd(s,CCU+fabric_offsets[i],&v) || (v&fabric_masks[i])!=s->config.fabric.fabric[i] ||
           (i && (v&(BIT(28)|BIT(27)))))return 0;
    }
    if((s->shared&A733_CLOCK_PERI0) && !peri0(s))return 0;
    return
        /* Video encode and decode gates closed, selector at most 6; bus word
         * gates closed and resets asserted. */
        rd(s,CCU+0xa80,&v) && !(v&BIT(31)) && ((v>>24)&7)<=6 &&
        rd(s,CCU+0xa88,&v) && !(v&BIT(31)) && ((v>>24)&7)<=6 &&
        check(s,CCU+0xa8c,0x50005,0) &&
        /* NPU module gate closed; bus gate closed, core, AXI and AHB resets
         * asserted, SRAM reset released. */
        rd(s,CCU+0xb00,&v) && !(v&BIT(31)) && ((v>>24)&7)<=6 &&
        check(s,CCU+0xb04,0xf0001,BIT(19)) &&
        /* DRAM0 gate on, DDRPLL, no update pending; the DRAM bus takes its
         * clock from the separate PHY PLL. */
        check(s,CCU+0xc00,BIT(31)|BIT(27)|UINT32_C(0x07010000),BIT(31)) &&
        check(s,CCU+0xc0c,BIT(16),BIT(16)) &&
        check(s,CCU+0x1b10,1,0) && check(s,CCU+0x1b14,1,0) && check(s,CCU+0x1b1c,1,0) &&
        rd(s,A733_NPU_PLL_CONTROL,&v) && a733_npu_pll_disabled(v) &&
        check(s,CCU+0x2a4,1,0) && check(s,CCU+0x2a8,BIT(31),0) &&
        check(s,CCU+0x2ac,UINT32_C(0xff100000),UINT32_C(0x30000000)) &&
        check(s,CCU+0x2b0,UINT32_C(0x1f0000),UINT32_C(0xb0000)) &&
        check(s,PCK,0xf,0) && check(s,PCK+8,0xf,0);
}
/* LOCK without the register lock: the exclusive PLL_NPU token makes this
 * task the only writer of the control word. At most 100 observations 3 us
 * apart, about 320 us with the 20 us settle, so the spin does not yield. */
static int lock_wait(struct a733_npu_startup *s)
{
    unsigned consecutive=0;
    uint64_t next=s->ops.spin_us(s->cookie);
    for(s->lock_samples=0;s->lock_samples<A733_NPU_PLL_SAMPLES;) {
        while(s->ops.spin_us(s->cookie)<next){}
        uint32_t v;
        if(!rd(s,A733_NPU_PLL_CONTROL,&v) || (v&~A733_NPU_PLL_LOCK)!=UINT32_C(0xe0305100))return 0;
        ++s->lock_samples;
        consecutive=(v&A733_NPU_PLL_LOCK) ? consecutive+1 : 0;
        next+=A733_NPU_PLL_SAMPLE_US;
        if(consecutive==3) {
            next+=A733_NPU_PLL_SETTLE_US-A733_NPU_PLL_SAMPLE_US;
            while(s->ops.spin_us(s->cookie)<next){}
            return now(s,0);
        }
    }
    return 0;
}
static int owner_task(const struct a733_npu_startup *s)
{
    return s && s->initialized && !s->failed && s->task &&
        xTaskGetSchedulerState()==taskSCHEDULER_RUNNING && xTaskGetCurrentTaskHandle()==s->task && privileged();
}
enum a733_npu_platform_result a733_npu_startup_step(struct a733_npu_startup *s)
{
    if(!owner_task(s))return RETAINED;
    ++s->calls;
    uint64_t t;
    if(!now(s,&t))return s->stage<A733_NPU_STARTUP_ENABLE ? refuse(s) : fault(s);
    const uint64_t deadline=s->config.deadline_us;
    for(;;) switch(s->stage) {
    case A733_NPU_STARTUP_TOKENS: {
        if(!s->clock.id) {
            enum a733_clock_result r=a733_clock_acquire(s->providers.clocks,s->config.owner,s->shared,
                A733_CLOCK_PLL_NPU|A733_CLOCK_NPU,deadline,&s->clock);
            if(r==A733_CLOCK_BUSY)return PENDING;
            if(r!=A733_CLOCK_OK)return refuse(s);
        }
        if(!s->rail.id) {
            enum a733_rail_result r=a733_rail_acquire(s->providers.rails,s->config.owner,
                A733_RAIL_BIT(A733_RAIL_DCDC2),0,deadline,&s->rail);
            if(r==A733_RAIL_BUSY)return PENDING;
            if(r!=A733_RAIL_PENDING && r!=A733_RAIL_READY)return refuse(s);
        }
        enum a733_rail_result rail=a733_rail_poll(s->providers.rails,&s->rail);
        if(rail==A733_RAIL_BUSY || rail==A733_RAIL_PENDING)return PENDING;
        if(rail!=A733_RAIL_READY)return refuse(s);
        if(!s->pck.token) {
            struct a733_gpu_power_resources ready={0};
            s->pck.mask=A733_GPU_RESOURCE_OSC|A733_GPU_RESOURCE_PCK|
                ((s->shared&A733_CLOCK_PERI0) ? A733_GPU_RESOURCE_PARENT : 0);
            s->pck.mode=A733_GPU_RESOURCE_STABLE;
            enum a733_gpu_resource_result r=a733_gpu_resources_acquire(s->providers.pck,s->config.pck_client,
                s->pck.mask,deadline,&s->pck.token,&ready);
            if(r==A733_GPU_RESOURCE_BUSY){s->pck.token=0;return PENDING;}
            if(r!=A733_GPU_RESOURCE_OK){s->pck.token=0;return refuse(s);}
        }
        /* The owner's done bit arrives within its own bound; anchor this wait
         * at whichever of now and the PMIC provider's startup deadline is later. */
        uint64_t anchor=s->providers.domains->pmic->config.startup_deadline;
        if(anchor<t)anchor=t;
        if(!a733_deadline_after(anchor,A733_POWER_SUPPLY_SETTLE_US+A733_POWER_SUPPLY_BUDGET_US+TICK_US,
           &s->wait_until))return refuse(s);
        s->stage=A733_NPU_STARTUP_SUPPLY;
        break;
    }
    case A733_NPU_STARTUP_SUPPLY: {
        const unsigned w=a733_power_authority_load(&s->providers.rails->authority);
        if(w&SUPPLY_FAULTS)return refuse(s);
        if(!(w&A733_POWER_SUPPLY_DONE))return t<s->wait_until ? PENDING : refuse(s);
        if(!(w&A733_POWER_DCDC2_VERIFIED))return refuse(s);
        s->stage=A733_NPU_STARTUP_ENABLE;
        break;
    }
    case A733_NPU_STARTUP_ENABLE: {
        int held=enter(s);
        if(held==0)return now(s,0) ? PENDING : refuse(s);
        if(held!=1)return fault(s);
        if(!now(s,0) || a733_power_domain_check(s->providers.domains,4)!=1 || !pass(s)) {
            s->ops.leave(s->cookie);s->register_held=0;return refuse(s);
        }
        if(!now(s,0) || a733_npu_apply(&counted,s,a733_npu_pll_enable,0,A733_NPU_PLL_ENABLE_STEPS,0)!=
           A733_NPU_PLL_ENABLE_STEPS)return fault(s);
        if(!leave(s) || !lock_wait(s))return fault(s);
        s->stage=A733_NPU_STARTUP_OPEN;
        break;
    }
    case A733_NPU_STARTUP_OPEN: {
        int held=enter(s);
        if(held!=1)return held==0 && now(s,0) ? PENDING : fault(s);
        uint32_t v;
        if(!now(s,0) || !rd(s,A733_NPU_PLL_CONTROL,&v) || !a733_npu_pll_locked(v) ||
           !check(s,CCU+0x1920,0x10001,0x10001) || !check(s,CCU+0x19a0,BIT(16),BIT(16)) ||
           a733_npu_apply(&counted,s,a733_npu_local_open,0,A733_NPU_LOCAL_OPEN_STEPS,0)!=A733_NPU_LOCAL_OPEN_STEPS ||
           !now(s,0))return fault(s);
        if(!leave(s))return fault(s);
        /* The row was admitted before the scheduler; a refusal of the request
         * itself means an earlier request is unanswered. */
        if(!a733_power_domain_request(&s->row,1) || !now(s,&t) ||
           !a733_deadline_after(t,A733_POWER_PCK_BOUND_US+2*TICK_US,&s->wait_until))return fault(s);
        s->stage=A733_NPU_STARTUP_ROW;
        return PENDING;
    }
    case A733_NPU_STARTUP_ROW: {
        const enum a733_power_row_state r=a733_power_domain_state(&s->row);
        if(r==A733_POWER_ROW_PENDING)return t<s->wait_until ? PENDING : fault(s);
        if(r!=A733_POWER_ROW_ON || !now(s,&t) || !a733_deadline_after(t,SETTLE_US,&s->wait_until))return fault(s);
        s->stage=A733_NPU_STARTUP_SETTLE;
        return PENDING;
    }
    case A733_NPU_STARTUP_SETTLE:
        /* No pinned source gives this settle; it is kept from the previous
         * controller and checked against the counter. */
        if(t<s->wait_until)return PENDING;
        s->stage=A733_NPU_STARTUP_FINAL;
        break;
    case A733_NPU_STARTUP_FINAL: {
        int held=enter(s);
        if(held!=1)return held==0 && now(s,0) ? PENDING : fault(s);
        uint32_t v;
        if(!now(s,0) || !check(s,PCK,0xf,8) || !check(s,PCK+8,0xf,8) ||
           !check(s,CCU+0xb04,0xf0001,0xf0001) || !check(s,CCU+0xb00,0x8700001f,BIT(31)) ||
           !rd(s,A733_NPU_PLL_CONTROL,&v) || (v&~A733_NPU_PLL_LOCK)!=UINT32_C(0xe8305100) ||
           !(v&A733_NPU_PLL_LOCK) || !now(s,0))return fault(s);
        if(!leave(s))return fault(s);
        s->stage=A733_NPU_STARTUP_READY;
        return READY;
    }
    case A733_NPU_STARTUP_READY:
        return READY;
    default:
        return fault(s);
    }
}
enum a733_npu_platform_result a733_npu_startup_stop(struct a733_npu_startup *s)
{
    if(!owner_task(s))return RETAINED;
    if(s->stage==A733_NPU_STARTUP_STOPPED)return READY;
    if(s->stage!=A733_NPU_STARTUP_READY || !now(s,0))return fault(s);
    int held=enter(s);
    if(held!=1)return held==0 && now(s,0) ? A733_NPU_PLATFORM_BUSY : fault(s);
    uint32_t v;
    if(!now(s,0) || !rd(s,A733_NPU_PLL_CONTROL,&v) || (v&~A733_NPU_PLL_LOCK)!=UINT32_C(0xe8305100) ||
       !check(s,PCK,0xf,8) || !check(s,PCK+8,0xf,8) ||
       !check(s,CCU+0xb00,0x8700001f,BIT(31)) || !check(s,CCU+0xb04,0xf0001,0xf0001) ||
       a733_npu_apply(&counted,s,a733_npu_local_stop,0,A733_NPU_LOCAL_STOP_STEPS,0)!=A733_NPU_LOCAL_STOP_STEPS ||
       !now(s,0))return fault(s);
    if(!leave(s))return fault(s);
    s->stage=A733_NPU_STARTUP_STOPPED;
    return READY;
}
int a733_npu_startup_ready(const struct a733_npu_startup *s)
{return s && s->initialized && !s->failed && s->stage==A733_NPU_STARTUP_READY;}
int a733_npu_startup_prepared(struct a733_npu_startup *s)
{
    if(!owner_task(s))return -1;
    if(s->stage<A733_NPU_STARTUP_READY)return 0;
    if(!a733_npu_startup_ready(s) ||
       atomic_load_explicit(&s->providers.counter->failed,memory_order_acquire) ||
       atomic_load_explicit(&s->providers.clocks->failed,memory_order_acquire) ||
       atomic_load_explicit(&s->providers.locks->retained,memory_order_acquire)) {
        (void)fault(s);return -1;
    }
    return 1;
}
int a733_npu_startup_init(struct a733_npu_startup *s,const struct a733_npu_startup_config *c,
    const struct a733_npu_startup_providers *p,const struct a733_npu_startup_ops *o,void *cookie)
{
    a733_clock_mask_t shared;
    if(!privileged() || xTaskGetSchedulerState()!=taskSCHEDULER_NOT_STARTED || !s || s->initialized ||
       !c || !p || !c->owner || !c->deadline_us || c->pck_client>=A733_GPU_RESOURCE_CLIENTS ||
       !a733_npu_platform_scopes(&c->fabric,&shared) ||
       !p->counter || !p->counter->initialized || !p->clocks || !p->clocks->initialized ||
       !p->locks || !p->locks->initialized || !p->pck || !p->rails || !p->domains ||
       p->domains->self!=p->domains || !p->domains->pmic ||
       (o && (!o->registers.read || !o->registers.write || !o->now || !o->spin_us || !o->enter ||
              !o->leave || !o->retain)) ||
       !a733_ranges_disjoint(s,sizeof(*s),c,sizeof(*c)) || !a733_ranges_disjoint(s,sizeof(*s),p,sizeof(*p)))return 0;
    const struct a733_npu_startup_ops native={{native_read,native_write},native_now,native_spin_us,
        native_enter,native_leave,native_retain};
    s->config=*c;s->providers=*p;s->ops=o ? *o : native;s->cookie=o ? cookie : s;s->shared=shared;
    if(!a733_power_domain_row_init(&s->row,p->domains,4,c->owner,&s->rail))return 0;
    s->initialized=1;return 1;
}
int a733_npu_startup_bind_task(struct a733_npu_startup *s,TaskHandle_t task)
{
    if(!s || !s->initialized || s->failed || s->task || !task || !privileged() ||
       xTaskGetSchedulerState()!=taskSCHEDULER_NOT_STARTED)return 0;
    s->task=task;return 1;
}
