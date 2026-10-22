#include "a733_clock_reference.h"
#include "a733_twi.h"
#include "a733_platform_locks.h"
#include "a733_counter_clock.h"
#include "task.h"
#include "a733_range.h"
#include "a733_mmio.h"
#ifndef SUNSTONE_CAMERA_MODEL
#include "a733_task_context.h"
#endif
#define CCU UINT32_C(0x02002000)
static struct a733_twi_native *instance;
#ifdef SUNSTONE_CAMERA_MODEL
extern int a733_camera_model_current(void *);
extern uint32_t a733_camera_model_load(uintptr_t);
extern void a733_camera_model_store(uintptr_t,uint32_t);
#endif
static int current(struct a733_twi_native *n)
{
#ifdef SUNSTONE_CAMERA_MODEL
    return n && n->self==n && instance==n && a733_camera_model_current(n->config.task);
#else
    return n && n->self==n && instance==n && a733_task_context_owner_el1((TaskHandle_t)n->config.task);
#endif
}
static int now(void *v,uint64_t *out)
{
    struct a733_twi_native *n=v;
    return n->config.locks->now(n->config.locks->context,out);
}
static int pin_health(struct a733_twi_native *n)
{
    /* A transaction holds immutable exclusive pin leases. No register mutex
     * may be nested under I2C. Inspect shared pin failure before acquisition
     * and again after releasing I2C, before any result can be taken. */
    if(n->held==A733_PLATFORM_I2C11_LOCK+1)return 1;
    if(n->held==1)return !n->config.pins->failed;
    if(!n->deadline ||
       !a733_platform_lock_enter(n->config.locks,A733_PLATFORM_REGISTER_LOCK,n->deadline))return 0;
    int ok=!n->config.pins->failed;
    a733_platform_lock_leave(n->config.locks,A733_PLATFORM_REGISTER_LOCK);return ok?1:-1;
}
static int healthy(struct a733_twi_native *n)
{
    if(!current(n) || n->fault)return -1;
    enum a733_clock_result c=a733_clock_token_status(n->config.clocks,&n->config.clock);
    enum a733_rail_result r=a733_rail_borrow_status(n->config.rails,n->config.rail);
    if(c==A733_CLOCK_BUSY || r==A733_RAIL_BUSY)return 0;
    return c==A733_CLOCK_OK && r==A733_RAIL_READY?pin_health(n):-1;
}
/* Registry or ledger BUSY is momentary metadata contention, not lost
 * authority. Once an access sequence has begun, repeat the observation in
 * place, without another register access, until the deadline. Only refusal,
 * failed time or expiry ends the sequence. */
static int settled(struct a733_twi_native *n,uint64_t d)
{
    for(;;) {
        int h=healthy(n);uint64_t t;
        if(h)return h;
        if(now(n,&t)!=1 || t>=d)return -1;
    }
}
static int authenticate(void *v)
{struct a733_twi_native *n=v;return !n->prepared?-1:n->held?settled(n,n->until):healthy(n);}
static int current_task(void *v){return current(v);}
static int held(struct a733_twi_native *n,unsigned which)
{
    return current(n) && n->held==which+1 && !n->fault &&
        !(atomic_load_explicit(&n->config.locks->retained,memory_order_acquire)&(1u<<which)) &&
        xSemaphoreGetMutexHolder(n->config.locks->mutex[which])==(TaskHandle_t)n->config.task;
}
static int enter(void *v,uint64_t deadline)
{
    struct a733_twi_native *n=v;int owned=0;
    /* Pin verification acquires the register mutex before the I2C mutex. */
    if(!n->prepared || n->held)return -1;
    int health=healthy(n);if(health!=1)return health;
    enum a733_pins_result p=a733_pins_held_banked(n->config.pins,&n->config.pin,&owned,deadline);
    if(p==A733_PINS_BUSY)return 0;
    if(p!=A733_PINS_OK || !owned)return -1;
    if(!a733_platform_lock_enter(n->config.locks,A733_PLATFORM_I2C11_LOCK,deadline))return 0;
    n->held=A733_PLATFORM_I2C11_LOCK+1;n->until=deadline;return 1;
}
static void leave(void *v)
{
    struct a733_twi_native *n=v;
    if(!held(n,A733_PLATFORM_I2C11_LOCK)){n->fault=1;return;}
    a733_platform_lock_leave(n->config.locks,A733_PLATFORM_I2C11_LOCK);n->held=0;
}
static void retain(void *v)
{
    struct a733_twi_native *n=v;
    if(n->held)configASSERT(a733_platform_lock_retain(n->config.locks,
        (enum a733_platform_lock)(n->held-1))==1);
    n->fault=1;
}
static int address(const struct a733_twi_native *n,uintptr_t a,unsigned writing)
{
    if(writing)return a==n->base+8 || a==n->base+0xc;
    if(a<n->base)return 0;
    switch(a-n->base) {
    case 8:case 0xc:case 0x10:case 0x14:case 0x18:case 0x1c:case 0x20:
    case 0x200:case 0x210:case 0x214:case 0x218:case 0x21c:return 1;
    default:return 0;
    }
}
static uint32_t load(uintptr_t a)
{
#ifdef SUNSTONE_CAMERA_MODEL
    return a733_camera_model_load(a);
#else
    return a733_mmio_read32_dsb_sy_both(a);
#endif
}
static void store(uintptr_t a,uint32_t v)
{
#ifdef SUNSTONE_CAMERA_MODEL
    a733_camera_model_store(a,v);
#else
    a733_mmio_write32_dsb_sy_both(a,v);
#endif
}
static int read32(void *v,uintptr_t a,uint32_t *out)
{
    struct a733_twi_native *n=v;
    if(!out || !held(n,A733_PLATFORM_I2C11_LOCK) || !address(n,a,0) || settled(n,n->until)!=1)return 0;
    *out=load(a);return settled(n,n->until)==1;
}
static int write32(void *v,uintptr_t a,uint32_t x)
{
    struct a733_twi_native *n=v;
    if(!held(n,A733_PLATFORM_I2C11_LOCK) || !address(n,a,1) || x>255 ||
       (a==n->base+0xc && x!=0x48 && x!=0x4c && x!=0x58 && x!=0x68) ||
       settled(n,n->until)!=1)return 0;
    store(a,x);return settled(n,n->until)==1;
}
int a733_twi_native_init(struct a733_twi_native *n,const struct a733_twi_native_config *c)
{
    if(!n || n->self || instance || !c || !c->clocks || !c->pins || !c->rails || !c->rail ||
       !c->locks || !c->locks->initialized || !c->task || c->locks->now!=a733_counter_clock_now ||
       c->clocks->now!=c->locks->now || c->clocks->context!=c->locks->context ||
       (c->apb1&~UINT32_C(0x0300001f)) || (c->apb1>>24!=0 && c->apb1>>24!=3) ||
       !a733_ranges_disjoint(n,sizeof(*n),c,sizeof(*c)) ||
       !a733_ranges_disjoint(n,sizeof(*n),c->clocks,sizeof(*c->clocks)) ||
       !a733_ranges_disjoint(n,sizeof(*n),c->pins,sizeof(*c->pins)) ||
       !a733_ranges_disjoint(n,sizeof(*n),c->rails,sizeof(*c->rails)) ||
       !a733_ranges_disjoint(n,sizeof(*n),c->rail,sizeof(*c->rail)) ||
       !a733_ranges_disjoint(n,sizeof(*n),c->locks,sizeof(*c->locks)) ||
       !a733_ranges_disjoint(n,sizeof(*n),c->locks->context,sizeof(struct a733_counter_clock)))return 0;
    if(c->controller!=A733_TWI_MAIN11 && c->controller!=A733_TWI_MAIN9)return 0;
    /* Both controllers run from APB1 (manual 4.1.6.156/158). */
    const unsigned nine=c->controller==A733_TWI_MAIN9,first=nine?14:7;
    const a733_clock_mask_t local=nine?A733_CLOCK_TWI9:A733_CLOCK_TWI11;
    uint32_t parent=c->apb1>>24==3?600000000u:24000000u,div=(c->apb1&31)+1;
    a733_clock_mask_t scopes=local|A733_CLOCK_FABRIC|A733_CLOCK_REF|A733_CLOCK_OSC;
    if(c->apb1>>24==3)scopes|=A733_CLOCK_PERI0;
    if(parent%div || c->apb1_hz!=parent/div ||
       (c->clock.exclusive&local)!=local ||
       ((c->clock.exclusive|c->clock.shared)&scopes)!=scopes ||
       (c->pin.masks[A733_PIN_E]&(3u<<first))!=(3u<<first) || !c->pin.owner ||
       c->pin.owner!=c->clock.owner || !c->rail->id)return 0;
    uint32_t divider,rate;
    if(!a733_twi_divider(c->apb1_hz,&divider,&rate))return 0;
    *n=(struct a733_twi_native){.self=n,.config=*c,.base=nine?A733_TWI9_BASE:A733_TWI11_BASE,
        .bgr=nine?A733_TWI9_BGR:A733_TWI11_BGR,.first_pin=first,.divider=divider,.rate_hz=rate};
    n->operations=(struct a733_twi_ops){now,authenticate,enter,leave,retain,read32,write32,current_task};
    instance=n;return 1;
}
/* Before the first store, BUSY makes prepare leave the register mutex and
 * stay pending. After it, the observation is repeated in place until d. */
static int before(struct a733_twi_native *n,uint64_t d)
{
    if(!held(n,A733_PLATFORM_REGISTER_LOCK))return 0;
    int h=n->stored?settled(n,d):healthy(n);
    if(!h)n->busy=1;
    uint64_t t;return h==1 && now(n,&t)==1 && t<d;
}
static int observed(struct a733_twi_native *n,uintptr_t a,uint32_t mask,uint32_t value,uint64_t d)
{return before(n,d) && (load(a)&mask)==value && before(n,d);}
static int update(struct a733_twi_native *n,uintptr_t a,uint32_t mask,uint32_t value,uint64_t d)
{
    if(!before(n,d))return 0;
    uint32_t old=load(a),next=(old&~mask)|(value&mask);
    if(!before(n,d))return 0;
    n->stored=1;store(a,next);return observed(n,a,UINT32_MAX,next,d);
}
int a733_twi_native_prepare(struct a733_twi_native *n,uint64_t d)
{
    int owned=0;
    if(!n || n->self!=n || n->prepared || n->fault || n->held || !d || !current(n))return -1;
    if(n->deadline && n->deadline!=d)return -1;
    n->deadline=d;
    int health=healthy(n);if(health!=1)return health;
    enum a733_pins_result p=a733_pins_held_banked(n->config.pins,&n->config.pin,&owned,d);
    if(p==A733_PINS_BUSY)return 0;
    if(p!=A733_PINS_OK || !owned)return -1;
    if(!a733_platform_lock_enter(n->config.locks,A733_PLATFORM_REGISTER_LOCK,d))return 0;
    n->held=1;
    if(!observed(n,n->bgr,0x10001,0,d) ||
       !observed(n,CCU+0x518,0x0300001f,n->config.apb1,d))goto fault;
    uint32_t v=load(0x07090160),ref=load(CCU),nn;
    /* TWI11 and TWI9 use APB1: SYS24M or PERI0, both normalized by PLL_REF. */
    if(!a733_clock_ref24_valid(v,ref) || !before(n,d))goto fault;
    if(n->config.apb1>>24==3) {
        v=load(CCU+0xa0);nn=((v>>8)&255)+1;
        if((v&0xfe000000)!=0xfe000000 || (v&0x01000002) || nn<53 || nn>105 ||
           24u*nn!=600u*(((v>>20)&7)+1)*2 ||
           !observed(n,CCU+0xa8,0x80000000,0,d) || !observed(n,CCU+0xac,0x8e000000,0,d) ||
           !observed(n,CCU+0x1908,0x0a000a00,0x0a000a00,d) ||
           !observed(n,CCU+0x1988,0x0a000000,0x0a000000,d))goto fault;
    }
    /* PE7/PE8 or PE14/PE15 mux words are read under the common register lock. */
    struct a733_pin_fields f;
    for(unsigned p=n->first_pin;p<=n->first_pin+1;p++)if(!a733_pin_fields_banked(A733_PIN_E,p,&f) ||
        !observed(n,f.mux_address,15u<<f.mux_shift,8u<<f.mux_shift,d))goto fault;
    const uintptr_t b=n->base;
    if(!update(n,n->bgr,0x10000,0x10000,d) || !update(n,n->bgr,1,1,d) ||
       !observed(n,b+0xc,0xfc,0,d) ||
       !observed(n,b+0x10,255,0xf8,d) ||
       !observed(n,b+0x200,0x80000003,0,d) ||
       !observed(n,b+0x214,0x000f0000,0,d) ||
       !observed(n,b+0x218,0x01000100,0,d) ||
       !observed(n,b+0x210,3,0,d) ||
       !observed(n,b+0x21c,0x007f007f,0,d) ||
       !observed(n,b+0x20,0x35,0x30,d) ||
       !observed(n,b+0x18,1,0,d) ||
       !update(n,b+0x14,0x1ff,n->divider,d) ||
       !update(n,b+0x1c,3,0,d) ||
       !update(n,b+0xc,0xfd,0x40,d))goto fault;
    a733_platform_lock_leave(n->config.locks,A733_PLATFORM_REGISTER_LOCK);n->held=0;
    uint64_t t;
    if(settled(n,d)!=1 || now(n,&t)!=1 || t>=d)goto fault;
    n->prepared=1;return 1;
fault:
    if(n->busy && !n->stored && n->held==1) {
        /* No register was written; retry the whole sequence later. */
        a733_platform_lock_leave(n->config.locks,A733_PLATFORM_REGISTER_LOCK);
        n->held=0;n->busy=0;return 0;
    }
    retain(n);return -1;
}
