/* SPDX-License-Identifier: GPL-2.0-or-later */
/* PCK600 sequencing follows the A733 domain driver.
 * Copyright(c) 2020 - 2023 Allwinner Technology Co.,Ltd. All rights reserved.
 * Copyright (c) 2021 ALLWINNER, Co. Ltd. */
#include "a733_hdmi_native.h"
#include "a733_platform_locks.h"
#include "a733_counter_clock.h"
#include "aarch64_device_memory.h"
#include "task.h"
#include <stdatomic.h>
#define CCU UINT32_C(0x02002000)
#define PCK UINT32_C(0x0706a000)
#define BIT(n) (UINT32_C(1)<<(n))
#define RAILS (A733_RAIL_BIT(A733_RAIL_DCDC2)|A733_RAIL_BIT(A733_RAIL_CLDO2))
#define SHARED (A733_CLOCK_OSC|A733_CLOCK_FABRIC)
_Static_assert(A733_HDMI_VO1_SCOPE==A733_CLOCK_VO1,"common VO1 scope");
enum { CLOCK,RAIL,RAIL_WAIT,PMIC_ID,SUPPLY0,SUPPLY1,MAPPINGS,PCK_ACQUIRE,BASELINE,RESET_ASSERT,RESET_HOLD,
       POWER_DELAY,POWER_ON,POWER_WAIT,CCU_PREPARE,COMPLETE };
static struct a733_hdmi_native *boot_owner;
static int equal(const void *a,const void *b,size_t n)
{const unsigned char *x=a,*y=b;for(size_t i=0;i<n;++i)if(x[i]!=y[i])return 0;return 1;}
static int separate(const void *a,size_t an,const void *b,size_t bn)
{uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;return a&&b&&an<=UINTPTR_MAX-x&&bn<=UINTPTR_MAX-y&&(x+an<=y||y+bn<=x);}
static int intact(struct a733_hdmi_native *n)
{return n&&n->self==n&&boot_owner==n&&equal(&n->config,&n->sealed,sizeof(n->config));}
static int current(struct a733_hdmi_native *n)
{
    uint64_t el,flags,nesting,critical;
    __asm__ volatile("mrs %0, CurrentEL\n mrs %1, daif":"=r"(el),"=r"(flags));
    if(!intact(n)||n->failed||el!=4||(flags&0x80))return 0;
    __asm__ volatile("msr daifset, #2":::"memory");
#if configNUMBER_OF_CORES > 1
    extern volatile uint64_t ullPortInterruptNestings[],ullCriticalNestings[];
    uint64_t core;__asm__ volatile("mrs %0, tpidr_el1":"=r"(core));
    nesting=core<configNUMBER_OF_CORES?ullPortInterruptNestings[core]:1;
    critical=core<configNUMBER_OF_CORES?ullCriticalNestings[core]:1;
#else
    extern uint64_t ullPortInterruptNesting;
    extern volatile uint64_t ullCriticalNesting;
    nesting=ullPortInterruptNesting;critical=ullCriticalNesting;
#endif
    __asm__ volatile("msr daif, %0"::"r"(flags):"memory");
    return !nesting&&!critical&&xTaskGetSchedulerState()==taskSCHEDULER_RUNNING&&
        xTaskGetCurrentTaskHandle()==(TaskHandle_t)n->config.task;
}
static int now(void *v,uint64_t *out)
{
    struct a733_hdmi_native *n=v;uint64_t t;
    if(!current(n)||!out||n->config.locks->now(n->config.locks->context,&t)!=1||t<n->last_now)return 0;
    n->last_now=t;*out=t;return 1;
}
static int timely(struct a733_hdmi_native *n)
{uint64_t t;return now(n,&t)&&t<n->deadline&&
    !atomic_load_explicit(&n->config.clocks->failed,memory_order_acquire);}
static int retain(void *v,const struct a733_hdmi_reservation *grant)
{
    struct a733_hdmi_native *n=v;(void)grant;
    if(!n||n->self!=n||boot_owner!=n)return -1;
    n->failed=1;
    if(n->held){
        if(a733_platform_lock_retain(n->sealed.locks,A733_PLATFORM_REGISTER_LOCK)!=1)return 0;
        n->held=0;
    }
    if(n->clock.id&&!n->clock_retained){
        enum a733_clock_result r=a733_clock_retain(n->sealed.clocks,&n->clock);
        if(r==A733_CLOCK_BUSY)return 0;
        if(r!=A733_CLOCK_OK&&r!=A733_CLOCK_FAULT)return -1;
        n->clock_retained=1;
    }
    if(n->rail.id&&!n->rail_retained){
        enum a733_rail_result r=a733_rail_retain(n->sealed.rails,&n->rail);
        if(r==A733_RAIL_BUSY)return 0;
        if(r!=A733_RAIL_RETAINED)return -1;
        n->rail_retained=1;
    }
    return 1;
}
static int fail(struct a733_hdmi_native *n)
{(void)retain(n,&n->grant);return -1;}
/* No manager call occurs while the global register mutex is held. */
static int tokens(struct a733_hdmi_native *n)
{
    return intact(n)&&!n->failed&&
        n->clock.owner==n->config.owner&&n->clock.shared==SHARED&&n->clock.exclusive==A733_CLOCK_VO1&&
        a733_clock_token_status(n->config.clocks,&n->clock)==A733_CLOCK_OK&&
        n->rail.owner==n->config.owner&&n->rail.shared==RAILS&&!n->rail.exclusive&&
        a733_rail_status(n->config.rails,&n->rail)==A733_RAIL_READY&&
        a733_gpu_resources_status(n->config.pck,n->config.pck_client,&n->pck_lease)==A733_GPU_RESOURCE_OK;
}
static int live(struct a733_hdmi_native *n)
{return tokens(n)&&a733_pmic_live(n->config.pmic,&n->pmic_lifetime)==A733_PMIC_READY;}
static int lock(struct a733_hdmi_native *n)
{
    if(!timely(n)||!tokens(n))return -1;
    int r=a733_platform_lock_enter(n->config.locks,A733_PLATFORM_REGISTER_LOCK,n->deadline);
    if(r!=1)return r==0?0:-1;
    n->held=1;
    return timely(n)?1:fail(n);
}
static int unlock(struct a733_hdmi_native *n)
{
    if(!timely(n))return fail(n);
    a733_platform_lock_leave(n->config.locks,A733_PLATFORM_REGISTER_LOCK);n->held=0;
    return timely(n)?1:fail(n);
}
static uint32_t load32(uintptr_t a)
{uint32_t x;__asm__ volatile("dsb sy":::"memory");x=*(volatile const uint32_t *)a;__asm__ volatile("dsb sy":::"memory");return x;}
static void store32(uintptr_t a,uint32_t x)
{__asm__ volatile("dsb sy":::"memory");*(volatile uint32_t *)a=x;__asm__ volatile("dsb sy":::"memory");}
static int update(struct a733_hdmi_native *n,uintptr_t a,uint32_t mask,uint32_t value)
{
    int r=lock(n);if(r!=1)return r;
    uint32_t old=load32(a),next=(old&~mask)|value;
    if(!timely(n))return fail(n);
    store32(a,next);
    if(!timely(n)||load32(a)!=next||!timely(n))return fail(n);
    return unlock(n);
}
static int snapshot(struct a733_hdmi_native *n,unsigned i)
{
    static const enum a733_rail rails[]={A733_RAIL_DCDC2,A733_RAIL_CLDO2};
    enum a733_pmic_status r;
    if(!n->requests[i].id){
        r=a733_pmic_submit(n->config.pmic,n->config.owner,&n->rail,rails[i],n->deadline,&n->requests[i]);
        if(r==A733_PMIC_BUSY)return 0;
        return r==A733_PMIC_PENDING?0:-1;
    }
    r=a733_pmic_result(n->config.pmic,&n->requests[i],&n->supplies[i]);
    if(r==A733_PMIC_PENDING||r==A733_PMIC_BUSY)return 0;
    struct a733_pmic_snapshot *s=&n->supplies[i];
    if(r!=A733_PMIC_COMPLETE||s->rail!=rails[i]||s->page_before||s->page_after||
        s->chip!=3||s->revision||s->enabled!=1||s->decoded!=1||
        s->programmed_uv!=(i?1800000u:800000u))return -1;
    /* Keep the real provider request and borrow until platform reset. */
    return 1;
}
static int mapped(uintptr_t a)
{
    struct aarch64_translation r,w;
    return aarch64_device_translate(a,0,&r)&&aarch64_device_translate(a,1,&w)&&
        r.physical==a&&w.physical==a&&r.attributes==0&&w.attributes==0;
}
static int baseline(struct a733_hdmi_native *n)
{
    int r=lock(n);if(r!=1)return r;
    uint32_t osc=load32(0x07090160),pck=load32(0x070101ac);
    if((((osc>>14)&3)!=0&&((osc>>14)&3)!=3)||!(pck&1))return fail(n);
    /* PCK token protects its source; these words verify the selected native
     * gates before accessing domain10. No unrelated domain is modified. */
    uint32_t control=load32(PCK),status=load32(PCK+8);
    if((control&15)!=(status&15)||((status&15)!=0&&(status&15)!=8))return fail(n);
    /* Alternative TV/eDP/HDCP/CEC producer clocks must be inactive. Cold
     * platform provenance additionally excludes already-running local output. */
    const uint16_t inactive[]={0x1640,0x1680,0x1684,0x1694};
    for(unsigned i=0;i<sizeof(inactive)/sizeof(*inactive);++i)
        if(load32(CCU+inactive[i])&BIT(31))return fail(n);
    const uint16_t buses[]={0x16cc,0x1604,0x160c,0x164c,0x168c};
    for(unsigned i=0;i<sizeof(buses)/sizeof(*buses);++i)
        if(load32(CCU+buses[i])&1)return fail(n);
    if(load32(CCU+0x1690)&BIT(31))return fail(n);
    n->domain_on=(status&15)==8;n->step=0;
    return unlock(n);
}
static int acquire(void *v,const struct a733_hdmi_request *request,uint64_t deadline,
                   struct a733_hdmi_reservation *out)
{
    struct a733_hdmi_native *n=v;
    if(!current(n)||!equal(request,&n->config.request,sizeof(*request))||
        out!=&n->controller.io.reservation)return fail(n);
    if(!n->deadline)n->deadline=deadline;
    if(deadline!=n->deadline||!timely(n))return fail(n);
    int r=0;
    switch(n->stage){
    case CLOCK:{
        enum a733_clock_result s=a733_clock_acquire(n->config.clocks,n->config.owner,SHARED,
            A733_CLOCK_VO1,n->deadline,&n->clock);
        if(s==A733_CLOCK_BUSY)return 0;
        if(s!=A733_CLOCK_OK)return fail(n);
        n->grant.clock=n->clock;*out=n->grant;++n->stage;break;}
    case RAIL:{
        enum a733_rail_result s=a733_rail_acquire(n->config.rails,n->config.owner,RAILS,0,n->deadline,&n->rail);
        if(s==A733_RAIL_BUSY)return 0;
        if(s!=A733_RAIL_PENDING)return fail(n);
        n->grant.rail_owner=n->rail.owner;n->grant.rail_generation=n->rail.id;*out=n->grant;++n->stage;break;}
    case RAIL_WAIT:{
        enum a733_rail_result s=a733_rail_poll(n->config.rails,&n->rail);
        if(s==A733_RAIL_BUSY||s==A733_RAIL_PENDING)return 0;
        if(s!=A733_RAIL_READY)return fail(n);
        ++n->stage;break;}
    case PMIC_ID:{
        enum a733_pmic_status s=a733_pmic_identity(n->config.pmic,&n->pmic_lifetime);
        if(s==A733_PMIC_BUSY)return 0;
        if(s!=A733_PMIC_READY)return fail(n);
        s=a733_pmic_live(n->config.pmic,&n->pmic_lifetime);
        if(s==A733_PMIC_BUSY||s==A733_PMIC_PENDING)return 0;
        if(s!=A733_PMIC_READY)return fail(n);
        ++n->stage;break;}
    case SUPPLY0:case SUPPLY1:
        r=snapshot(n,n->stage-SUPPLY0);if(r<0)return fail(n);if(r)++n->stage;break;
    case PCK_ACQUIRE:{
        n->pck_lease.mask=A733_GPU_RESOURCE_OSC|A733_GPU_RESOURCE_PCK;
        n->pck_lease.mode=A733_GPU_RESOURCE_STABLE;
        enum a733_gpu_resource_result s=a733_gpu_resources_acquire(n->config.pck,
            n->config.pck_client,n->pck_lease.mask,n->deadline,&n->pck_lease.token,&n->pck_ready);
        if(s==A733_GPU_RESOURCE_BUSY)return 0;
        if(s!=A733_GPU_RESOURCE_OK)return fail(n);
        ++n->stage;break;}
    case MAPPINGS:{
        static const uintptr_t pages[]={CCU,CCU+0x1000,0x03000000,0x05510000,
            0x05520000,0x05521000,0x05522000,0x05523000,0x05524000,0x05525000,
            0x05526000,0x05527000,0x05600000,0x05730000,0x07010000,PCK,0x07090000};
        if(!mapped(pages[n->step]))return fail(n);
        if(++n->step==sizeof(pages)/sizeof(*pages)){n->step=0;++n->stage;}break;}
    case BASELINE:
        r=baseline(n);if(r<0)return fail(n);if(r)++n->stage;break;
    case RESET_ASSERT:{
        static const uint16_t offsets[]={0x16ec,0x16cc,0x1604,0x168c};
        uint32_t mask=n->step==3?BIT(16)|BIT(17):BIT(16);
        r=update(n,CCU+offsets[n->step],mask,0);
        if(r<0)return fail(n);
        if(r&&++n->step==4){
            if(n->last_now>UINT64_MAX-10)return fail(n);
            n->reset_until=n->last_now+10;++n->stage;
        }break;}
    case RESET_HOLD:
        if(n->last_now>=n->reset_until){n->step=n->domain_on?5u:0u;++n->stage;}break;
    case POWER_DELAY:{
        static const uint16_t offsets[]={0x170,0x174,0xc00,0xc04,0xc10};
        static const uint32_t values[]={0x1f1f1f,0x1f1f,0x08080808,0x0808,8};
        if(n->step==5){++n->stage;break;}
        r=update(n,PCK+offsets[n->step],UINT32_MAX,values[n->step]);
        if(r<0)return fail(n);if(r)++n->step;break;}
    case POWER_ON:
        if(n->last_now>UINT64_MAX-10000)return fail(n);
        n->power_deadline=n->last_now+10000;
        r=update(n,PCK,15,8);if(r<0)return fail(n);if(r)++n->stage;break;
    case POWER_WAIT:{
        if(n->last_now>=n->power_deadline)return fail(n);
        r=lock(n);if(r<0)return fail(n);if(!r)break;
        uint32_t x=load32(PCK+8);
        if(!timely(n)||n->last_now>=n->power_deadline)return fail(n);
        r=unlock(n);if(r<0)return fail(n);
        if((x&15)==8){n->step=0;++n->stage;}break;}
    case CCU_PREPARE:{
        static const struct {uint16_t offset;uint32_t mask,value;} steps[]={
            {0x5c0,BIT(4),BIT(4)}, {0x16ec,BIT(16),BIT(16)},
            {0x16cc,BIT(16),BIT(16)}, {0x1604,BIT(16),BIT(16)},
            {0x168c,BIT(16)|BIT(17),BIT(16)|BIT(17)},
            {0x1690,0x07000000,0x01000000},
            {0x16cc,1,1},{0x1604,1,1},{0x168c,1,1},{0x1690,BIT(31),BIT(31)}};
        r=update(n,CCU+steps[n->step].offset,steps[n->step].mask,steps[n->step].value);
        if(r<0)return fail(n);
        if(r&&++n->step==sizeof(steps)/sizeof(*steps))++n->stage;break;}
    case COMPLETE:
        if(!live(n)||!timely(n))return fail(n);
        *out=n->grant;return 1;
    default:return fail(n);
    }
    return timely(n)?0:fail(n);
}
static int validate(void *v,const struct a733_hdmi_request *r,
                    const struct a733_hdmi_reservation *g,uint64_t deadline)
{
    struct a733_hdmi_native *n=v;
    if(!current(n)||n->stage!=COMPLETE||!equal(r,&n->config.request,sizeof(*r))||
        !equal(g,&n->grant,sizeof(*g)))return fail(n);
    n->deadline=deadline;
    return live(n)&&timely(n)?1:fail(n);
}
static int allowed(uintptr_t a,unsigned width,unsigned writing)
{
    if(width==1)return a>=A733_HDMI_BASE&&a<A733_HDMI_BASE+0x8000;
    if(width!=4||(a&3))return 0;
    if(a==0x03000160||a==0x03000164)return 1;
    if(a==A733_HDMI_TOP_BASE||a==A733_HDMI_TOP_BASE+0x20)return 1;
    if(a>=A733_HDMI_PHY_BASE&&a<=A733_HDMI_PHY_BASE+0x34)return 1;
    if(!writing&&a==A733_HDMI_PHY_BASE+0x40)return 1;
    if(a>=A733_HDMI_TV_BASE&&a<=A733_HDMI_TV_BASE+0x340)return 1;
    return 0;
}
static int read32(void *v,uintptr_t a,uint32_t *out)
{
    struct a733_hdmi_native *n=v;
    if(!out||!allowed(a,4,0)||!live(n)||lock(n)!=1)return fail(n);
    *out=load32(a);return unlock(n);
}
static int write32(void *v,uintptr_t a,uint32_t value)
{
    struct a733_hdmi_native *n=v;
    if(!allowed(a,4,1)||!live(n)||lock(n)!=1)return fail(n);
    store32(a,value);return unlock(n);
}
static int read8(void *v,uintptr_t a,uint8_t *out)
{
    struct a733_hdmi_native *n=v;
    if(!out||!allowed(a,1,0)||!live(n)||lock(n)!=1)return fail(n);
    __asm__ volatile("dsb sy":::"memory");*out=*(volatile const uint8_t *)a;
    __asm__ volatile("dsb sy":::"memory");return unlock(n);
}
static int write8(void *v,uintptr_t a,uint8_t value)
{
    struct a733_hdmi_native *n=v;
    if(!allowed(a,1,1)||!live(n)||lock(n)!=1)return fail(n);
    __asm__ volatile("dsb sy":::"memory");*(volatile uint8_t *)a=value;
    __asm__ volatile("dsb sy":::"memory");return unlock(n);
}
static int barrier(void *v)
{struct a733_hdmi_native *n=v;if(!current(n)||!timely(n))return fail(n);__asm__ volatile("dsb sy":::"memory");return 1;}
int a733_hdmi_native_init(struct a733_hdmi_native *n,const struct a733_hdmi_native_config *c)
{
    if(xTaskGetSchedulerState()!=taskSCHEDULER_NOT_STARTED||!n||n->self||boot_owner||!c||!c->clocks||!c->rails||!c->pmic||!c->locks||!c->pck||
        !c->task||!c->owner||!c->epoch||!c->recovery||!c->request.initial.id||!c->request.initial.generation||
        c->pck_client>=A733_GPU_RESOURCE_CLIENTS||
        !c->locks->initialized||c->locks->now!=a733_counter_clock_now||
        !separate(n,sizeof(*n),c->locks->context,sizeof(struct a733_counter_clock))||
        !separate(c,sizeof(*c),c->locks->context,sizeof(struct a733_counter_clock))||
        c->clocks->now!=c->locks->now||c->clocks->context!=c->locks->context||
        c->rails->binding.clocks!=c->clocks||c->rails->binding.gpu!=c->pck||
        c->pmic->self!=c->pmic||c->pmic->clocks!=c->clocks||c->pmic->rails!=c->rails||
        !c->pmic->ops.outside||c->pmic->ops.outside(c->pmic->cookie,n,sizeof(*n))!=1||
        c->pmic->ops.outside(c->pmic->cookie,c,sizeof(*c))!=1)return 0;
    const void *objects[]={n,c,c->clocks,c->rails,c->pmic,c->locks,c->pck};
    const size_t sizes[]={sizeof(*n),sizeof(*c),sizeof(*c->clocks),sizeof(*c->rails),
        sizeof(*c->pmic),sizeof(*c->locks),sizeof(*c->pck)};
    for(unsigned i=0;i<7;++i)for(unsigned j=i+1;j<7;++j)
        if(!separate(objects[i],sizes[i],objects[j],sizes[j]))return 0;
    n->self=n;n->config=*c;n->sealed=*c;
    n->grant=(struct a733_hdmi_reservation){.generation=c->epoch,.system_owner=c->owner,
        .system_generation=c->epoch,.inactive_epoch=c->epoch,.recovery_epoch=c->recovery};
    n->controller_config=(struct a733_hdmi_config){
        .ops={now,acquire,validate,read8,write8,read32,write32,barrier,retain},.context=n,.request=c->request};
    /* Constructor is serialized before scheduling. Irreversible boot ownership
     * includes failed construction; no second image task can reclaim it. */
    boot_owner=n;
    if(a733_hdmi_init(&n->controller,&n->controller_config)!=1){n->failed=1;return 0;}
    return 1;
}
