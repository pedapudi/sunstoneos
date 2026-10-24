/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "a733_typec_native.h"
#include "a733_platform_locks.h"
#include "a733_counter_clock.h"
#include "a733_clock_reference.h"
#include "a733_mapping_check.h"
#include "a733_mmio.h"
#include "a733_range.h"
#include "a733_task_context.h"
#include "task.h"
#include <stddef.h>
#define BASE UINT32_C(0x07084000)
#define BGR UINT32_C(0x0701019c)
#define APBS1 UINT32_C(0x07010010)
#define RAILS (A733_RAIL_BIT(A733_RAIL_DCDC1)|A733_RAIL_BIT(A733_RAIL_DC1SW2))
#define SHARED (A733_CLOCK_APBS1|A733_CLOCK_REF|A733_CLOCK_OSC)
static struct a733_typec_native *instance;
static int same(const void *a,const void *b,size_t bytes)
{const unsigned char *x=a,*y=b;for(size_t i=0;i<bytes;i++)if(x[i]!=y[i])return 0;return 1;}
static int intact(struct a733_typec_native *n)
{return n&&n->self==n&&instance==n&&same(&n->config,&n->sealed,sizeof(n->config));}
static int current(void *p)
{struct a733_typec_native *n=p;return intact(n)&&a733_task_context_owner_el1(n->config.provider_task);}
static int now(void *p,uint64_t *out)
{struct a733_typec_native *n=p;return intact(n)&&n->config.locks->now(n->config.locks->context,out)==1;}
static int outside(void *p,const void *x,unsigned long bytes)
{
    struct a733_typec_native *n=p;
    return intact(n)&&a733_ranges_disjoint(n,sizeof(*n),x,bytes)&&
        a733_ranges_disjoint(n->config.clocks,sizeof(*n->config.clocks),x,bytes)&&
        a733_ranges_disjoint(n->config.rails,sizeof(*n->config.rails),x,bytes)&&
        a733_ranges_disjoint(n->config.pins,sizeof(*n->config.pins),x,bytes)&&
        a733_ranges_disjoint(n->config.pmic,sizeof(*n->config.pmic),x,bytes)&&
        a733_ranges_disjoint(n->config.locks,sizeof(*n->config.locks),x,bytes)&&
        a733_ranges_disjoint(n->config.locks->context,sizeof(struct a733_counter_clock),x,bytes)&&
        a733_pmic_outside(n->config.pmic,x,bytes)==1;
}
static int ownership(struct a733_typec_native *n,unsigned require_pin)
{
    if(!intact(n)||n->clock.owner!=n->config.electrical.owner||n->clock.shared!=SHARED||
       n->clock.exclusive!=A733_CLOCK_S_TWI1||n->rail.owner!=n->clock.owner||
       (require_pin&&n->pin.owner!=n->clock.owner))return -1;
    enum a733_clock_result c=a733_clock_token_status(n->config.clocks,&n->clock);
    enum a733_rail_result r=a733_rail_status(n->config.rails,&n->rail);
    if(c==A733_CLOCK_BUSY||r==A733_RAIL_BUSY)return 0;
    if(c!=A733_CLOCK_OK||r!=A733_RAIL_READY)return -1;
    return 1;
}
static int authority(struct a733_typec_native *n,unsigned require_pin)
{
    int owned=ownership(n,require_pin);if(owned!=1)return owned;
    enum a733_pmic_status m=a733_pmic_live(n->config.pmic,&n->pmic_lifetime);
    return m==A733_PMIC_READY?1:m==A733_PMIC_BUSY?0:-1;
}
static int live(void *p)
{struct a733_typec_native *n=p;return current(n)&&!n->setup_fault&&!n->bus_fault?authority(n,1):-1;}
static int bus_current(void *p){return current(p);}
static int bus_auth(void *p)
{struct a733_typec_native *n=p;return n->stage==9?live(n):-1;}
static int bus_enter(void *p,uint64_t d)
{
    struct a733_typec_native *n=p;int held=0,r=live(n);
    if(r!=1)return r;
    if(n->held)return -1;
    enum a733_pins_result pins=a733_pins_held_banked(n->config.pins,&n->pin,&held,d);
    if(pins==A733_PINS_BUSY)return 0;
    if(pins!=A733_PINS_OK||!held)return -1;
    if(!a733_platform_lock_enter(n->config.locks,A733_PLATFORM_I2C1_LOCK,d))return 0;
    n->held=(unsigned)A733_PLATFORM_I2C1_LOCK+1;return 1;
}
static int bus_owned(struct a733_typec_native *n)
{return current(n)&&n->held==(unsigned)A733_PLATFORM_I2C1_LOCK+1&&!n->bus_fault&&live(n)==1&&
    !atomic_load_explicit(&n->config.clocks->failed,memory_order_acquire)&&
    !(atomic_load_explicit(&n->config.locks->retained,memory_order_acquire)&(1u<<A733_PLATFORM_I2C1_LOCK))&&
    xSemaphoreGetMutexHolder(n->config.locks->mutex[A733_PLATFORM_I2C1_LOCK])==(TaskHandle_t)n->config.provider_task;}
static void bus_leave(void *p)
{struct a733_typec_native *n=p;if(!bus_owned(n)){n->bus_fault=1;return;}
 a733_platform_lock_leave(n->config.locks,A733_PLATFORM_I2C1_LOCK);n->held=0;}
static void bus_retain(void *p)
{struct a733_typec_native *n=p;if(!current(n))return;
 if(n->held)configASSERT(a733_platform_lock_retain(n->config.locks,(enum a733_platform_lock)(n->held-1))==1);
 n->bus_fault=1;a733_typec_revoke(&n->provider);}
static int bus_read(void *p,uintptr_t a,uint32_t *out)
{
    if(!out||!bus_owned(p)||a<BASE)return 0;
    switch(a-BASE){case 8:case 0xc:case 0x10:case 0x14:case 0x18:case 0x1c:case 0x20:
    case 0x200:case 0x210:case 0x214:case 0x218:case 0x21c:*out=a733_mmio_read32_dsb_sy_both(a);return 1;default:return 0;}
}
static int bus_write(void *p,uintptr_t a,uint32_t v)
{
    if(!bus_owned(p)||v>255||(a!=BASE+8&&a!=BASE+0xc)||
       (a==BASE+0xc&&v!=0x48&&v!=0x4c&&v!=0x58&&v!=0x68))return 0;
    a733_mmio_write32_dsb_sy_both(a,v);return 1;
}
static int at(uintptr_t a)
{
    static const struct a733_mapping_rule device={A733_MAPPING_READ_WRITE,1,{{A733_MAIR_DEVICE_NGNRNE,A733_SH_ANY}},0,1};
    return a733_mapping_check(aarch64_device_translate,a,a,&device);
}
static int before(struct a733_typec_native *n)
{
    uint64_t t;
    return current(n)&&now(n,&t)&&!atomic_load_explicit(&n->config.clocks->failed,memory_order_acquire)&&
        a733_deadline_admit(t,&n->last_now,n->deadline);
}
static int observed(struct a733_typec_native *n,uintptr_t a,uint32_t mask,uint32_t v)
{return before(n)&&(a733_mmio_read32_dsb_sy_both(a)&mask)==v&&before(n);}
static int update(struct a733_typec_native *n,uintptr_t a,uint32_t mask,uint32_t value)
{if(!before(n))return 0;uint32_t v=(a733_mmio_read32_dsb_sy_both(a)&~mask)|(value&mask);if(!before(n))return 0;
 a733_mmio_write32_dsb_sy_both(a,v);return observed(n,a,UINT32_MAX,v);}
static int snapshot(struct a733_typec_native *n,unsigned i)
{
    enum a733_rail rail=i?A733_RAIL_DC1SW2:A733_RAIL_DCDC1;
    if(!n->requests[i].id){enum a733_pmic_status r=a733_pmic_submit(n->config.pmic,n->clock.owner,&n->rail,rail,n->deadline,&n->requests[i]);return r==A733_PMIC_BUSY||r==A733_PMIC_PENDING?0:-1;}
    enum a733_pmic_status r=a733_pmic_result(n->config.pmic,&n->requests[i],&n->supplies[i]);
    if(r==A733_PMIC_BUSY||r==A733_PMIC_PENDING)return 0;
    struct a733_pmic_snapshot *s=&n->supplies[i];
    return r==A733_PMIC_COMPLETE&&s->rail==rail&&!s->page_before&&!s->page_after&&
        s->chip==3&&!s->revision&&s->decoded==1&&s->enabled==1&&s->programmed_uv==3300000?1:-1;
}
static int prepare(void *p,uint64_t d)
{
    struct a733_typec_native *n=p;int r=0,owned=0;
    if(!current(n)||n->setup_fault||n->held)return -1;
    n->deadline=d;if(!before(n))goto fault;
    switch(n->stage){
    case 0:{enum a733_clock_result x=a733_clock_acquire(n->config.clocks,n->config.electrical.owner,SHARED,A733_CLOCK_S_TWI1,d,&n->clock);
        if(x==A733_CLOCK_BUSY)return 0;if(x!=A733_CLOCK_OK)goto fault;++n->stage;break;}
    case 1:{enum a733_rail_result x=a733_rail_acquire(n->config.rails,n->config.electrical.owner,RAILS,0,d,&n->rail);
        if(x==A733_RAIL_BUSY)return 0;if(x!=A733_RAIL_PENDING)goto fault;++n->stage;break;}
    case 2:{enum a733_rail_result x=a733_rail_poll(n->config.rails,&n->rail);
        if(x==A733_RAIL_BUSY||x==A733_RAIL_PENDING)return 0;if(x!=A733_RAIL_READY)goto fault;++n->stage;break;}
    case 3:{enum a733_pmic_status x=a733_pmic_identity(n->config.pmic,&n->pmic_lifetime);
        if(x==A733_PMIC_BUSY)return 0;if(x!=A733_PMIC_READY)goto fault;
        x=a733_pmic_live(n->config.pmic,&n->pmic_lifetime);
        if(x==A733_PMIC_BUSY||x==A733_PMIC_PENDING)return 0;if(x!=A733_PMIC_READY)goto fault;++n->stage;break;}
    case 4:case 5:r=snapshot(n,n->stage-4);if(r<0)goto fault;if(r)++n->stage;break;
    case 6:{
        static const uintptr_t pages[]={BASE,0x07010000,0x07025000,0x07090000};
        if(!at(pages[n->index]))goto fault;if(++n->index==4){n->index=0;++n->stage;}break;}
    case 7:{
        r=authority(n,0);if(r<0)goto fault;if(!r)return 0;
        enum a733_pins_result x=a733_pins_acquire_banked(n->config.pins,n->clock.owner,n->pin_requests,4,&n->pin,d);
        if(x==A733_PINS_BUSY)return 0;if(x!=A733_PINS_OK)goto fault;
        atomic_store_explicit(&n->gpio_ready,1,memory_order_release);++n->stage;break;}
    case 8:
        r=authority(n,1);if(r<0)goto fault;if(!r)return 0;
        {enum a733_pins_result x=a733_pins_held_banked(n->config.pins,&n->pin,&owned,d);
         if(x==A733_PINS_BUSY)return 0;if(x!=A733_PINS_OK||!owned)goto fault;}
        if(!a733_platform_lock_enter(n->config.locks,A733_PLATFORM_REGISTER_LOCK,d))return 0;
        n->held=1;
        /* S_TWI1 shares TWI0's register set. The PMU registers of S_TWI0 are
         * absent from its manual register map and are never probed here. */
        if(!observed(n,APBS1,0x0700001f,0)||!observed(n,BGR,0x20002,0)||!before(n))goto fault;
        /* APBS1 source0 is raw DCXO, not the normalized PLL_REF output.
         * The shared OSC/APBS1 leases and remote exclusion retain this image. */
        uint32_t oscillator=a733_mmio_read32_dsb_sy_both(UINT32_C(0x07090160));
        if(!before(n)||(oscillator&UINT32_C(0x10000002))!=UINT32_C(0x10000002))goto fault;
        n->input_hz=a733_clock_dcxo_hz(oscillator);
        if(!a733_twi_divider(n->input_hz,&n->divider,&n->rate_hz))goto fault;
        if(!update(n,BGR,0x20000,0x20000)||!update(n,BGR,2,2)||
           !observed(n,BASE+0xc,0xfc,0)||!observed(n,BASE+0x10,255,0xf8)||
           !observed(n,BASE+0x200,0x80000003,0)||!observed(n,BASE+0x210,3,0)||
           !observed(n,BASE+0x214,0xffff0000,0)||!observed(n,BASE+0x218,0x01000100,0)||
           !observed(n,BASE+0x21c,0x007f007f,0)||!observed(n,BASE+0x18,1,0)||
           !observed(n,BASE+0x20,0x35,0x30)||!update(n,BASE+0x14,255,n->divider)||
           !update(n,BASE+0x1c,3,0)||!update(n,BASE+0xc,0xfd,0x40))goto fault;
        a733_platform_lock_leave(n->config.locks,A733_PLATFORM_REGISTER_LOCK);n->held=0;
        if(!before(n)||!a733_twi_init_controller(&n->bus,&n->bus_ops,n,n->divider,A733_TWI_R1,1))goto fault;
        ++n->stage;break;
    case 9:r=live(n);if(r<0||!before(n))goto fault;return r;
    default:goto fault;
    }
    if(!before(n))goto fault;return 0;
fault:
    if(n->held)configASSERT(a733_platform_lock_retain(n->config.locks,(enum a733_platform_lock)(n->held-1))==1);
    n->setup_fault=1;a733_typec_revoke(&n->provider);return -1;
}
static int vbus(void *p,unsigned on,uint64_t d)
{
    struct a733_typec_native *n=p;
    if(!current(n)||on>1||!atomic_load_explicit(&n->gpio_ready,memory_order_acquire))return -1;
    if(on&&(live(n)!=1||atomic_load_explicit(&n->provider.revoked,memory_order_acquire)))return -1;
    unsigned command=atomic_load_explicit(&n->command,memory_order_acquire);
    if(command){
        unsigned r=atomic_load_explicit(&n->result,memory_order_acquire);
        if(!r)return 0;
        atomic_store_explicit(&n->command,0,memory_order_release);
        if(r!=1)return -1;
        return atomic_load_explicit(&n->output_state,memory_order_acquire)==(on?2u:1u)?1:0;
    }
    atomic_store_explicit(&n->result,0,memory_order_relaxed);
    atomic_store_explicit(&n->command_deadline,d,memory_order_relaxed);
    atomic_store_explicit(&n->command,on?2u:1u,memory_order_release);return 0;
}
static int irq(void *p,unsigned *level,uint64_t d)
{
    struct a733_typec_native *n=p;uint32_t value;
    if(!current(n)||n->held||live(n)!=1||!level)return -1;
    enum a733_pins_result r=a733_pins_read_inputs_banked(n->config.pins,&n->pin,A733_PIN_L,1u<<9,&value,d);
    if(r==A733_PINS_BUSY)return 0;if(r!=A733_PINS_OK)return -1;*level=!!(value&(1u<<9));return 1;
}
int a733_typec_native_init(struct a733_typec_native *n,const struct a733_typec_native_config *c)
{
    if(!n||n->self||instance||!c||xTaskGetSchedulerState()!=taskSCHEDULER_NOT_STARTED||
       !c->clocks||!c->pins||!c->rails||!c->pmic||!c->locks||!c->provider_task||!c->power_task||
       c->provider_task==c->power_task||!c->locks->initialized||c->locks->now!=a733_counter_clock_now||
       c->clocks->now!=c->locks->now||c->clocks->context!=c->locks->context||
       c->rails->binding.clocks!=c->clocks||c->pmic->self!=c->pmic||c->pmic->clocks!=c->clocks||
       c->pmic->rails!=c->rails||c->pmic->pins!=c->pins||
       a733_pmic_outside(c->pmic,n,sizeof(*n))!=1||
       a733_pmic_outside(c->pmic,c,sizeof(*c))!=1)return 0;
    const void *objects[]={n,c,c->clocks,c->rails,c->pins,c->pmic,c->locks,c->locks->context};
    const size_t sizes[]={sizeof(*n),sizeof(*c),sizeof(*c->clocks),sizeof(*c->rails),sizeof(*c->pins),sizeof(*c->pmic),sizeof(*c->locks),sizeof(struct a733_counter_clock)};
    for(unsigned i=0;i<8;i++)for(unsigned j=i+1;j<8;j++)if(!a733_ranges_disjoint(objects[i],sizes[i],objects[j],sizes[j]))return 0;
    /* Cold GPIO input/disabled admission only; an inherited output could already
     * be sourcing a partner and must be handled by a separate recovery path. */
    for(unsigned i=0;i<4;i++)if(c->expected[i].function!=0&&c->expected[i].function!=15)return 0;
    n->self=n;n->config=*c;n->sealed=*c;
    const unsigned pins[]={8,9,12,13};
    for(unsigned i=0;i<4;i++)n->pin_requests[i]=(struct a733_pin_request_banked){
        .bank=A733_PIN_L,.pin=pins[i],.expected=c->expected[i],
        .desired={i==0?1u:i==1?0u:3u,0,0},.output_value=0};
    n->bus_ops=(struct a733_twi_ops){now,bus_auth,bus_enter,bus_leave,bus_retain,bus_read,bus_write,bus_current};
    atomic_init(&n->gpio_ready,0);atomic_init(&n->command,0);atomic_init(&n->result,0);
    atomic_init(&n->output_state,0);atomic_init(&n->command_deadline,0);
    struct a733_typec_config cfg=c->electrical;
    cfg.ops=(struct a733_typec_ops){outside,now,current,prepare,live,vbus,irq};cfg.context=n;cfg.bus=&n->bus;
    instance=n;return a733_typec_init(&n->provider,&cfg);
}
static int power_stop(struct a733_typec_native *n,uint64_t t)
{
    a733_typec_revoke(&n->provider);
    if(!n->power_shutdown_deadline){
        if(!a733_deadline_after(t,5000,&n->power_shutdown_deadline)){n->power_fault=1;return 0;}
    }
    if(t>=n->power_shutdown_deadline&&atomic_load_explicit(&n->output_state,memory_order_acquire)!=1){n->power_fault=1;return 0;}
    return 1;
}
int a733_typec_native_power_poll(struct a733_typec_native *n)
{
    if(!intact(n)||!a733_task_context_owner_el1(n->config.power_task))return -1;
    if(n->power_fault)return -1;
    if(!atomic_load_explicit(&n->gpio_ready,memory_order_acquire))return 0;
    uint64_t t,d;
    if(!now(n,&t)||!a733_deadline_after(t,1000,&d)||!a733_monotonic_advance(t,&n->power_now)){
        a733_typec_revoke(&n->provider);n->power_fault=1;return -1;}
    unsigned command=atomic_load_explicit(&n->command,memory_order_acquire);
    unsigned output=atomic_load_explicit(&n->output_state,memory_order_acquire);
    unsigned stop=atomic_load_explicit(&n->provider.revoked,memory_order_acquire)||n->power_fault;
    if(output==2||command==2){
        uint64_t progress=atomic_load_explicit(&n->provider.progress_until,memory_order_acquire);
        uint64_t power=atomic_load_explicit(&n->provider.power_until,memory_order_acquire);
        if(!progress||!power||t>=progress||t>=power)stop=1;
    }
    /* Shutdown cannot depend on a live PMIC task: its shared I2C mutex may
     * already be retained. The permanent clock/rail/pin ownership remains. */
    int owned=ownership(n,1);
    if(owned!=1){
        (void)power_stop(n,t);
        if(owned<0)n->power_fault=1;
        return -1;
    }
    if(output==2&&!stop){
        uint32_t level=0;
        enum a733_pins_result x=a733_pins_read_inputs_banked(n->config.pins,&n->pin,A733_PIN_L,1u<<9,&level,d);
        if(x!=A733_PINS_OK||!(level&(1u<<9)))stop=1;
        if(x!=A733_PINS_OK&&x!=A733_PINS_BUSY){(void)power_stop(n,t);n->power_fault=1;return -1;}
    }
    if(stop&&!power_stop(n,t))return -1;
    if(stop&&d>n->power_shutdown_deadline)d=n->power_shutdown_deadline;
    unsigned desired=stop?1:command;
    if(desired==1&&output==1){
        atomic_store_explicit(&n->result,stop&&command==2?2u:1u,memory_order_release);
    }else if(desired&&(!atomic_load_explicit(&n->result,memory_order_acquire)||(stop&&output!=1))){
        if(desired==2&&t>=atomic_load_explicit(&n->command_deadline,memory_order_acquire)){
            if(!power_stop(n,t))return -1;desired=1;stop=1;
        }
        enum a733_pins_result r=a733_pins_write_outputs(n->config.pins,&n->pin,A733_PIN_L,1u<<8,desired==2?1u<<8:0,d);
        if(r!=A733_PINS_OK){
            (void)power_stop(n,t);
            if(r!=A733_PINS_BUSY){n->power_fault=1;atomic_store_explicit(&n->result,2,memory_order_release);}
            return -1;
        }
        atomic_store_explicit(&n->output_state,desired,memory_order_release);
        atomic_store_explicit(&n->result,stop&&command==2?2u:1u,memory_order_release);
    }
    return stop?-1:1;
}
