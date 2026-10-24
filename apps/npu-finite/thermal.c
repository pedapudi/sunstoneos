/* One bootstrap-owned polling THS controller. A work-admission STOP permits
 * configured sensor shutdown; uncertain register/clock state never does. */
#include "thermal.h"
#include "a733_thermal_native.h"
#include "a733_mapping_check.h"
#include "a733_mmio.h"
#include <string.h>
unsigned npu_finite_thermal_irq_admitted __attribute__((section(".npu_runtime_retained")));
#define ALLOW UINT64_C(1)
#define ACCEPTED UINT64_C(2)
#define STOP UINT64_C(4)
#define SHIFT 6u
#define FRESH_US UINT64_C(1000000)
#define DRAIN_US UINT64_C(5000000)
static int now(void *v,uint64_t *n)
{struct npu_finite_thermal *t=v;return a733_counter_clock_now(t->clock,n);}
static int owner(struct npu_finite_thermal *t)
{return xTaskGetCurrentTaskHandle()==t->owner && !atomic_load_explicit(&t->fault,memory_order_acquire) &&
    !atomic_load_explicit(&t->clock->failed,memory_order_acquire) &&
    !atomic_load_explicit(&t->registry->failed,memory_order_acquire);}
static const struct a733_mapping_rule device={A733_MAPPING_WRITE,1,{{A733_MAIR_DEVICE_NGNRNE,A733_SH_OUTER}},0,1};
static int mapped(uintptr_t a){return a733_mapping_check(aarch64_device_translate,a,a,&device);}
static int irq_quiet(void)
{
    const uintptr_t addresses[]={0x03400000,0x03400108,0x03400208,0x03400308};
    uint32_t values[4];
    for(unsigned i=0;i<4;++i) {
        if(!mapped(addresses[i]))return 0;
        values[i]=a733_mmio_read32_dsb_sy_both(addresses[i]);
    }
    return !(values[0]&UINT32_C(0x80000000)) && !((values[1]|values[2]|values[3])&128);
}
static int authorize(void *v,const struct a733_thermal_config *c,const struct a733_clock_token *token)
{
    struct npu_finite_thermal *t=v;
    const uintptr_t addresses[]={0x02002518,0x02002fc0,0x02002fe4,0x02522000,0x03006244,0x03006248,0x0300624c,0x07090160};
    if(!owner(t) || !npu_finite_thermal_irq_admitted || c->owner!=t->config.owner || c->handoff!=t->config.handoff ||
       c->startup_deadline_us!=t->config.startup_deadline_us || c->apb1!=t->config.apb1 ||
       c->apb1_hz!=t->config.apb1_hz || c->configure_adc_clock!=t->config.configure_adc_clock ||
       c->alarm_mask || c->critical_mask || memcmp(c->limits,t->config.limits,sizeof(c->limits)) ||
       token->id!=t->token.id || token->owner!=t->token.owner || token->shared!=t->token.shared ||
       token->exclusive!=t->token.exclusive || a733_clock_token_status(t->registry,&t->token)!=A733_CLOCK_OK)return 0;
    for(unsigned i=0;i<sizeof(addresses)/sizeof(*addresses);++i)if(!mapped(addresses[i]))return 0;
    return 1;
}
static int enter(void *v,uint64_t d)
{struct npu_finite_thermal *t=v;return owner(t)?a733_platform_lock_enter(t->locks,A733_PLATFORM_REGISTER_LOCK,d):-1;}
static void leave(void *v)
{struct npu_finite_thermal *t=v;a733_platform_lock_leave(t->locks,A733_PLATFORM_REGISTER_LOCK);}
static void retain(void *v)
{struct npu_finite_thermal *t=v;(void)a733_platform_lock_retain(t->locks,A733_PLATFORM_REGISTER_LOCK);}
static int read_register(void *v,uintptr_t a,uint32_t *out)
{struct npu_finite_thermal *t=v;return owner(t)&&t->driver.held&&a733_thermal_native_read(NULL,a,out);}
static int write_register(void *v,uintptr_t a,uint32_t value)
{struct npu_finite_thermal *t=v;return owner(t)&&t->driver.held&&a733_thermal_native_write(NULL,a,value);}
static void stop(struct npu_finite_thermal *t,unsigned reason,uint64_t n)
{
    uint64_t old=atomic_load_explicit(&t->gate,memory_order_acquire);
    while(!(old&STOP)) {
        uint64_t end=n>=t->deadline || t->deadline-n<DRAIN_US?t->deadline:n+DRAIN_US;
        uint64_t next=(end<<SHIFT)|STOP|(old&ACCEPTED)|((uint64_t)reason<<3);
        if(atomic_compare_exchange_weak_explicit(&t->gate,&old,next,memory_order_acq_rel,memory_order_acquire))return;
    }
}
static int watch_at(struct npu_finite_thermal *t,uint64_t n)
{
    uint64_t word=atomic_load_explicit(&t->gate,memory_order_acquire);
    if(n>=t->deadline)return 0;
    if(!(word&STOP) && n>=(word>>SHIFT))stop(t,NPU_THERMAL_STALE,n);
    word=atomic_load_explicit(&t->gate,memory_order_acquire);
    return !(word&STOP) || n<(word>>SHIFT);
}
int npu_finite_thermal_watch(struct npu_finite_thermal *t)
{uint64_t n;return now(t,&n)&&watch_at(t,n);}
int npu_finite_thermal_stopping(const struct npu_finite_thermal *t)
{return (atomic_load_explicit(&t->gate,memory_order_acquire)&STOP)!=0;}
unsigned npu_finite_thermal_reason(const struct npu_finite_thermal *t)
{return (unsigned)(atomic_load_explicit(&t->gate,memory_order_acquire)>>3)&7;}
int npu_finite_thermal_claim(struct npu_finite_thermal *t)
{
    uint64_t n;if(!now(t,&n)||!watch_at(t,n))return 0;
    uint64_t old=atomic_load_explicit(&t->gate,memory_order_acquire);
    while((old&(ALLOW|ACCEPTED|STOP))==ALLOW && n<(old>>SHIFT)) {
        if(atomic_compare_exchange_weak_explicit(&t->gate,&old,old|ACCEPTED,memory_order_acq_rel,memory_order_acquire)) {
            /* Preemption after the first read cannot admit an expired sample. */
            if(now(t,&n)&&n<(old>>SHIFT)&&n<t->deadline)return 1;
            atomic_fetch_and_explicit(&t->gate,~ACCEPTED,memory_order_acq_rel);
            stop(t,NPU_THERMAL_STALE,n);return 0;
        }
    }
    return 0;
}
void npu_finite_thermal_complete(struct npu_finite_thermal *t)
{atomic_fetch_and_explicit(&t->gate,~ACCEPTED,memory_order_acq_rel);}
void npu_finite_thermal_close(struct npu_finite_thermal *t)
{uint64_t n;if(now(t,&n))stop(t,NPU_THERMAL_NONE,n);}
static int fault(struct npu_finite_thermal *t)
{stop(t,NPU_THERMAL_FAULT,t->observed);atomic_store_explicit(&t->fault,1,memory_order_release);return 0;}
static int notify(void *v,enum a733_thermal_level level)
{struct npu_finite_thermal *t=v;if(level==A733_THERMAL_CRITICAL)stop(t,NPU_THERMAL_CRITICAL,t->observed);return 1;}
int npu_finite_thermal_init(struct npu_finite_thermal *t,const struct a733_npu_thermal_boot *b,
    struct a733_counter_clock *clock,struct a733_clock_leases *registry,struct a733_platform_locks *locks,TaskHandle_t task,uint64_t deadline)
{
    uint64_t n;
    if(!t||!b||!task||!npu_finite_thermal_irq_admitted||deadline>(UINT64_MAX>>SHIFT)||
       !a733_counter_clock_now(clock,&n)||n>=deadline||deadline-n<=FRESH_US)return 0;
    t->clock=clock;t->registry=registry;t->locks=locks;t->owner=task;t->deadline=deadline;t->initial_until=n+FRESH_US;
    atomic_init(&t->gate,t->initial_until<<SHIFT);atomic_init(&t->stopped,0);atomic_init(&t->fault,0);
    t->config=(struct a733_thermal_config){.owner=b->owner,.handoff=b->authority,.startup_deadline_us=t->initial_until,
        .apb1=b->apb1,.apb1_hz=b->apb1_hz,.configure_adc_clock=b->configure_adc_clock};
    a733_clock_mask_t shared=A733_CLOCK_FABRIC|A733_CLOCK_REF|A733_CLOCK_OSC,exclusive=A733_CLOCK_THS;
    if(b->configure_adc_clock)exclusive|=A733_CLOCK_GPADC24M;else shared|=A733_CLOCK_GPADC24M;
    if(b->apb1>>24==3)shared|=A733_CLOCK_PERI0;
    if(a733_clock_acquire(registry,b->owner,shared,exclusive,deadline,&t->token)!=A733_CLOCK_OK)return 0;
    const struct a733_thermal_ops ops={now,authorize,enter,leave,retain,read_register,write_register};
    return a733_thermal_init(&t->driver,&t->config,&ops,t,registry,&t->token);
}
int npu_finite_thermal_step(struct npu_finite_thermal *t)
{
    if(!now(t,&t->observed)||!owner(t)||t->observed>=t->deadline)return fault(t);
    if(!watch_at(t,t->observed)||!irq_quiet())return fault(t);
    if(npu_finite_thermal_stopping(t)) {
        if(atomic_load_explicit(&t->stopped,memory_order_acquire))return 1;
        if(!t->active)return fault(t); /* Unfinished controller startup cannot be declared stopped. */
        uint64_t end=atomic_load_explicit(&t->gate,memory_order_acquire)>>SHIFT;
        enum a733_thermal_result r=a733_thermal_stop(&t->driver,end);
        if(r==A733_THERMAL_STOPPED&&irq_quiet()){atomic_store_explicit(&t->stopped,1,memory_order_release);return 1;}
        if(r==A733_THERMAL_BUSY&&!t->driver.fault)return 1;
        return fault(t);
    }
    if(!t->active) {
        enum a733_thermal_result r=a733_thermal_poll(&t->driver);
        if(t->driver.fault)return fault(t);
        if(r==A733_THERMAL_PENDING||r==A733_THERMAL_BUSY)return 1;
        if(r!=A733_THERMAL_READY||!a733_thermal_policy_init(&t->policy,t->observed,notify,t))return fault(t);
        t->active=1;
    }
    if(t->observed<t->next_read)return 1;
    struct a733_thermal_sample sample;
    enum a733_thermal_result r=a733_thermal_read(&t->driver,t->deadline,&sample);
    if(t->driver.fault||!now(t,&t->observed))return fault(t);
    enum a733_thermal_level level=a733_thermal_policy_update(&t->policy,t->observed,r,&sample);
    if(level!=A733_THERMAL_NORMAL) {
        stop(t,level==A733_THERMAL_CRITICAL?NPU_THERMAL_CRITICAL:t->policy.passive?NPU_THERMAL_HOT:NPU_THERMAL_STALE,t->observed);
    } else if(a733_thermal_policy_seen(&t->policy)==31) {
        uint64_t fresh=UINT64_MAX;
        for(unsigned i=0;i<5;++i)if(t->policy.seen_us[i]<fresh)fresh=t->policy.seen_us[i];
        fresh=fresh>t->deadline-FRESH_US?t->deadline:fresh+FRESH_US;
        uint64_t old=atomic_load_explicit(&t->gate,memory_order_acquire);
        while(!(old&STOP)) {
            uint64_t next=(fresh<<SHIFT)|ALLOW|(old&ACCEPTED);
            if(atomic_compare_exchange_weak_explicit(&t->gate,&old,next,memory_order_acq_rel,memory_order_acquire))break;
        }
    }
    t->next_read=t->observed+25000;
    return 1;
}
uint64_t npu_finite_thermal_deadline(const struct npu_finite_thermal *t)
{uint64_t word=atomic_load_explicit(&t->gate,memory_order_acquire);return word&STOP?word>>SHIFT:0;}
int npu_finite_thermal_quiet(const struct npu_finite_thermal *t)
{return !atomic_load_explicit(&t->fault,memory_order_acquire) && atomic_load_explicit(&t->stopped,memory_order_acquire) && irq_quiet();}
