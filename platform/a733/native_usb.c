#include "a733_native_usb.h"
#include "a733_usb_platform_native.h"
#include <stddef.h>
#include <string.h>
static int now(void *p,uint64_t *v)
{return a733_counter_clock_now(&((struct a733_native_usb *)p)->counter,v);}
static int gpu_now(void *p,uint64_t *v)
{return a733_counter_clock_now(&((struct a733_native_usb *)((char *)p - offsetof(struct a733_native_usb,gpu_native)))->counter,v);}
static int enter(void *p,uint64_t deadline)
{return ((struct a733_native_usb *)p)->live(((struct a733_native_usb *)p)->context)&&a733_platform_lock_enter(&((struct a733_native_usb *)p)->locks,A733_PLATFORM_REGISTER_LOCK,deadline);}
static void leave(void *p)
{a733_platform_lock_leave(&((struct a733_native_usb *)p)->locks,A733_PLATFORM_REGISTER_LOCK);}
static void retain(void *p)
{struct a733_native_usb *n=p;(void)a733_platform_lock_retain(&n->locks,A733_PLATFORM_REGISTER_LOCK);n->fail(n->context);}
/* The USB task owns both tokens from acquisition through permanent retention.
 * Startup calls this only after clock acquisition and all supply snapshots.
 * Runtime callers additionally require the final PREPARED publication. */
static int usb_provider_bad(struct a733_native_usb *n,unsigned reason,int result)
{
    if(!n->usb_provider_denial){n->usb_provider_denial=reason;n->usb_provider_result=result;}
    return -1;
}
static int usb_providers(struct a733_native_usb *n)
{
    if(!n->live(n->context))return usb_provider_bad(n,A733_NATIVE_USB_PROVIDER_LIVE,0);
    if(xTaskGetCurrentTaskHandle()!=n->usb_task)return usb_provider_bad(n,A733_NATIVE_USB_PROVIDER_TASK,0);
    if(atomic_load_explicit(&n->locks.retained,memory_order_acquire))
        return usb_provider_bad(n,A733_NATIVE_USB_PROVIDER_RETAINED_LOCK,0);
    if(n->usb_rail_index!=4)return usb_provider_bad(n,A733_NATIVE_USB_PROVIDER_RAIL_INDEX,0);
    if(n->usb.token.owner!=n->boot.usb.owner)return usb_provider_bad(n,A733_NATIVE_USB_PROVIDER_CLOCK_OWNER,0);
    if(n->usb_rail.owner!=n->boot.usb.owner)return usb_provider_bad(n,A733_NATIVE_USB_PROVIDER_RAIL_OWNER,0);
    enum a733_clock_result c=a733_clock_token_status(&n->clocks,&n->usb.token);
    if(c==A733_CLOCK_BUSY)return 0;
    if(c!=A733_CLOCK_OK)return usb_provider_bad(n,A733_NATIVE_USB_PROVIDER_CLOCK,c);
    enum a733_rail_result r=a733_rail_status(&n->rails,&n->usb_rail);
    if(r==A733_RAIL_BUSY)return 0;
    if(r!=A733_RAIL_READY)return usb_provider_bad(n,A733_NATIVE_USB_PROVIDER_RAIL,r);
    enum a733_pmic_status p=a733_pmic_live(&n->pmic,&n->usb_power);
    if(p==A733_PMIC_BUSY)return 0;
    if(p!=A733_PMIC_READY)return usb_provider_bad(n,A733_NATIVE_USB_PROVIDER_PMIC,p);
    return n->live(n->context)?1:usb_provider_bad(n,A733_NATIVE_USB_PROVIDER_LIVE,0);
}
int a733_native_usb_health(struct a733_native_usb *n)
{
    if(!n||xTaskGetCurrentTaskHandle()!=n->usb_task||
       !atomic_load_explicit(&n->usb_prepared,memory_order_acquire)||
       !n->usb.initialized||n->usb.self!=(uintptr_t)&n->usb||n->usb.fault)return -1;
    return usb_providers(n);
}
static int usb_authorize(void *p,const struct a733_usb_platform_config *c,const struct a733_clock_token *t)
{
    struct a733_native_usb *n=p;
    const struct a733_usb_platform_config *b=&n->boot.usb;
    if(!n->live(n->context)||c!=&n->usb.config||c->owner!=b->owner||c->handoff!=b->handoff||
       c->deadline_us!=b->deadline_us||c->cold_reference!=b->cold_reference||
       memcmp(c->fabric,b->fabric,sizeof(c->fabric))||memcmp(c->rate_hz,b->rate_hz,sizeof(c->rate_hz))||
       t!=&n->usb.token||t->owner!=c->owner)return -1;
    return usb_providers(n);
}
static int usb_denied(struct a733_native_usb *n,unsigned reason,int status)
{
    if(!n->usb_io_denial){n->usb_io_denial=reason;n->usb_io_provider_status=status;}
    return 0;
}
static int usb_io_ready(struct a733_native_usb *n)
{
    if(!n->live(n->context))return usb_denied(n,A733_NATIVE_USB_DENIAL_LIVE,0);
    if(xTaskGetCurrentTaskHandle()!=n->usb_task)return usb_denied(n,A733_NATIVE_USB_DENIAL_TASK,0);
    if(xSemaphoreGetMutexHolder(n->locks.mutex[0])!=n->usb_task)
        return usb_denied(n,A733_NATIVE_USB_DENIAL_MUTEX,0);
    int result=usb_providers(n);
    /* Metadata contention issues no MMIO. The provider may retry a poll only
     * before a successful write; partial reset/write stages stay retained. */
    if(result==0)return A733_USB_PLATFORM_IO_BUSY;
    return result==1 ? 1 : usb_denied(n,A733_NATIVE_USB_DENIAL_PROVIDER,result);
}
static int usb_read(void *p,uintptr_t a,uint32_t *v)
{
    struct a733_native_usb *n=p;int ready=usb_io_ready(n);
    if(ready!=1)return ready;
    return a733_usb_platform_native_read(NULL,a,v)==1 ? 1 :
        usb_denied(n,A733_NATIVE_USB_DENIAL_ADAPTER,0);
}
static int usb_write(void *p,uintptr_t a,uint32_t v)
{
    struct a733_native_usb *n=p;int ready=usb_io_ready(n);
    if(ready!=1)return ready;
    return a733_usb_platform_native_write(NULL,a,v)==1 ? 1 :
        usb_denied(n,A733_NATIVE_USB_DENIAL_ADAPTER,0);
}
int a733_native_usb_step(struct a733_native_usb *n)
{
    static const enum a733_rail rail[4]={A733_RAIL_DCDC1,A733_RAIL_DC1SW2,A733_RAIL_DCDC9,A733_RAIL_ELDO6};
    if(!n->live(n->context))return -1;
    if(!atomic_load_explicit(&n->pmic_ready,memory_order_acquire))return 0;
    if(!n->usb_step) {
        a733_rail_mask_t mask=0;for(unsigned i=0;i<4;++i)mask|=A733_RAIL_BIT(rail[i]);
        enum a733_rail_result r=a733_rail_acquire(&n->rails,n->boot.usb.owner,mask,0,n->boot.usb.deadline_us,&n->usb_rail);
        if(r==A733_RAIL_BUSY)return 0;if(r!=A733_RAIL_PENDING)return -1;n->usb_step=1;return 0;
    }
    if(n->usb_step==1) {
        enum a733_rail_result r=a733_rail_poll(&n->rails,&n->usb_rail);
        if(r==A733_RAIL_BUSY||r==A733_RAIL_PENDING)return 0;if(r!=A733_RAIL_READY)return -1;
        enum a733_pmic_status q=a733_pmic_identity(&n->pmic,&n->usb_power);
        if(q==A733_PMIC_BUSY)return 0;if(q!=A733_PMIC_READY)return -1;n->usb_step=2;return 0;
    }
    if(n->usb_step==2) {
        /* Queue all four snapshots so the provider reads them back to back.
         * Results are still checked and reaped in rail order; a failure
         * leaves every outstanding borrow in place. */
        for(unsigned i=0;i<4;++i) {
            if(n->usb_request[i].id)continue;
            enum a733_pmic_status r=a733_pmic_submit(&n->pmic,n->boot.usb.owner,&n->usb_rail,
                rail[i],n->boot.usb.deadline_us,&n->usb_request[i]);
            if(r==A733_PMIC_BUSY)return 0;
            if(r!=A733_PMIC_PENDING)return -1;
            if(!n->usb_request[i].id)return 0;
        }
        n->usb_step=3;return 0;
    }
    if(n->usb_step==3) {
        struct a733_pmic_snapshot *s=&n->usb_snapshot[n->usb_rail_index];
        enum a733_pmic_status r=a733_pmic_result(&n->pmic,&n->usb_request[n->usb_rail_index],s);
        if(r==A733_PMIC_BUSY||r==A733_PMIC_PENDING)return 0;
        if(r!=A733_PMIC_COMPLETE||s->rail!=rail[n->usb_rail_index]||s->page_before||s->page_after||
           s->chip!=3||s->revision||s->decoded!=1||s->enabled!=1||
           s->programmed_uv!=n->boot.usb_supply_uv[n->usb_rail_index])return -1;
        n->usb_step=4;return 0;
    }
    if(n->usb_step==4) {
        enum a733_pmic_status r=a733_pmic_reap(&n->pmic,&n->usb_request[n->usb_rail_index]);
        if(r==A733_PMIC_BUSY)return 0;if(r!=A733_PMIC_COMPLETE)return -1;
        n->usb_request[n->usb_rail_index]=(struct a733_pmic_request){0};
        n->usb_step=++n->usb_rail_index==4?5:3;return 0;
    }
    enum a733_usb_platform_result r=a733_usb_platform_poll(&n->usb);
    if(r==A733_USB_PLATFORM_PENDING||r==A733_USB_PLATFORM_BUSY)return 0;
    if(r!=A733_USB_PLATFORM_PREPARED)return -1;
    atomic_store_explicit(&n->usb_prepared,1,memory_order_release);return 1;
}
int a733_native_usb_init(struct a733_native_usb *n,const struct a733_native_usb_config *b,
    int (*live)(void *),void (*fail)(void *),void *context)
{
    if(!n||!b||!live||!fail||n->used||!b->identity||!b->usb.owner||!b->pmic.owner||
       b->usb.owner==b->pmic.owner||b->usb.handoff!=b->identity||b->pmic.identity!=b->identity||
       b->usb_supply_uv[0]!=3300000||b->usb_supply_uv[1]!=3300000||
       b->usb_supply_uv[2]<500000||b->usb_supply_uv[2]>1820000||
       b->usb_supply_uv[3]<500000||b->usb_supply_uv[3]>1500000||
       b->usb_supply_uv[2]<=b->usb_supply_uv[3])return 0;
    n->used=1;n->boot=*b;n->live=live;n->fail=fail;n->context=context;
    atomic_init(&n->pmic_ready,0);atomic_init(&n->usb_prepared,0);
    if(!a733_counter_clock_init(&n->counter,24000000)||
       !a733_platform_locks_init(&n->locks,a733_counter_clock_now,&n->counter)||
       !a733_clock_leases_init(&n->clocks,a733_counter_clock_now,&n->counter))return 0;
    struct a733_pins_locks pl={enter,leave,now,n};
    if(!a733_pins_native_init(&n->pin_native,&pl)||
       !a733_pins_init_banks(&n->pins,&n->pin_native.operations,&n->pin_native,&n->boot.pins)||
       !a733_gpu_resources_native_init(&n->gpu_native,&n->locks.callbacks))return 0;
    n->gpu_native.operations.now=gpu_now;
    if(!a733_gpu_resources_init(&n->gpu,&n->gpu_native.operations,&n->gpu_native))return 0;
    struct a733_rail_binding rb={&n->clocks,&n->gpu,b->identity,a733_counter_clock_now,&n->counter};
    if(!a733_rail_leases_init(&n->rails,&rb))return 0;
    return 1;
}
int a733_native_usb_bind(struct a733_native_usb *n,TaskHandle_t pmic,TaskHandle_t usb)
{
    if(!n||!n->used||!pmic||!usb||pmic==usb||n->pmic_task||n->usb_task)return 0;
    n->pmic_task=pmic;n->usb_task=usb;
    if(!a733_pmic_native_init(&n->pmic_native,&n->locks,pmic)||
       !a733_pmic_native_bind(&n->pmic,&n->boot.pmic,&n->pmic_native,&n->clocks,&n->rails,&n->pins)||
       !a733_gpu_resources_native_use_pmic(&n->gpu_native,&n->gpu,&n->pmic))return 0;
    struct a733_usb_platform_ops ops={now,usb_authorize,enter,leave,retain,usb_read,usb_write};
    return a733_usb_platform_init(&n->usb,&n->boot.usb,&ops,n,&n->clocks);
}
