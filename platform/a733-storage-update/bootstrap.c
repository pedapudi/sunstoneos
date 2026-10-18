#define A733_STORAGE_NATIVE_IMPLEMENTATION
#include "a733_storage_update_native.h"
#include "a733_usb_platform_native.h"
#include "task.h"
#include <string.h>

static struct a733_storage_native native;
int a733_storage_native_task_privileged(void (*entry)(void *),void *context)
{
    return native.used&&context==&native&&(entry==a733_storage_native_pmic_task||
        entry==a733_storage_native_worker_task||entry==a733_storage_native_usb_task);
}
static int nonzero(const uint8_t *p,size_t n)
{unsigned v=0;for(size_t i=0;i<n;++i)v|=p[i];return v!=0;}
static int now(void *p,uint64_t *v)
{return a733_counter_clock_now(&((struct a733_storage_native *)p)->counter,v);}
static int gpu_now(void *p,uint64_t *v)
{return p==&native.gpu_native&&a733_counter_clock_now(&native.counter,v);}
static int enter(void *p,uint64_t deadline)
{return a733_storage_native_live(p)&&a733_platform_lock_enter(&((struct a733_storage_native *)p)->locks,A733_PLATFORM_REGISTER_LOCK,deadline);}
static void leave(void *p)
{a733_platform_lock_leave(&((struct a733_storage_native *)p)->locks,A733_PLATFORM_REGISTER_LOCK);}
static void retain(void *p)
{struct a733_storage_native *n=p;(void)a733_platform_lock_retain(&n->locks,A733_PLATFORM_REGISTER_LOCK);a733_storage_native_fail(n);}
void a733_storage_native_fail(struct a733_storage_native *n)
{
    atomic_store_explicit(&n->failed,1,memory_order_release);
    atomic_store_explicit(&n->transport.lost,1,memory_order_seq_cst);
    a733_storage_channel_lost(&n->channel);
    a733_maintenance_revoke(n->maintenance);
    /* Only the storage worker mutates service state. Its atomic loss input
     * remains valid even before initialization and never becomes reusable. */
}
static int mmc_admit(void *p,const struct a733_mmc_platform_config *c,const struct a733_clock_token *t)
{
    struct a733_storage_native *n=p;
    const struct a733_mmc_platform_config *b=&n->boot.mmc;
    if(!a733_storage_native_live(n)||c!=&n->mmc.config||c->owner!=b->owner||c->handoff!=b->handoff||
       c->topology!=b->topology||c->deadline_us!=b->deadline_us||c->lifetime_deadline_us!=b->lifetime_deadline_us||
       c->kind!=b->kind||c->emmc_host!=b->emmc_host||c->voltage_mask!=b->voltage_mask||
       memcmp(c->fabric,b->fabric,sizeof(c->fabric))||memcmp(c->fabric_hz,b->fabric_hz,sizeof(c->fabric_hz))||
       memcmp(c->expected,b->expected,sizeof(c->expected))||memcmp(c->desired,b->desired,sizeof(c->desired))||
       t->owner!=c->owner||a733_clock_token_status(&n->clocks,t)!=A733_CLOCK_OK)return -1;
    return 1;
}
/* The USB task owns both tokens from acquisition through permanent retention.
 * Startup calls this only after clock acquisition and all supply snapshots.
 * Runtime callers additionally require the final PREPARED publication. */
static int usb_providers(struct a733_storage_native *n)
{
    if(!a733_storage_native_live(n)||xTaskGetCurrentTaskHandle()!=n->task_handle[2]||
       atomic_load_explicit(&n->locks.retained,memory_order_acquire)||
       n->usb_rail_index!=4||n->usb.token.owner!=n->boot.usb.owner||
       n->usb_rail.owner!=n->boot.usb.owner)return -1;
    enum a733_clock_result c=a733_clock_token_status(&n->clocks,&n->usb.token);
    if(c==A733_CLOCK_BUSY)return 0;if(c!=A733_CLOCK_OK)return -1;
    enum a733_rail_result r=a733_rail_status(&n->rails,&n->usb_rail);
    if(r==A733_RAIL_BUSY)return 0;if(r!=A733_RAIL_READY)return -1;
    enum a733_pmic_status p=a733_pmic_live(&n->pmic,&n->usb_power);
    if(p==A733_PMIC_BUSY)return 0;if(p!=A733_PMIC_READY)return -1;
    return a733_storage_native_live(n)?1:-1;
}
int a733_storage_native_usb_health(struct a733_storage_native *n)
{
    if(!n||xTaskGetCurrentTaskHandle()!=n->task_handle[2]||
       !atomic_load_explicit(&n->usb_prepared,memory_order_acquire)||
       !n->usb.initialized||n->usb.self!=(uintptr_t)&n->usb||n->usb.fault)return -1;
    return usb_providers(n);
}
static int usb_authorize(void *p,const struct a733_usb_platform_config *c,const struct a733_clock_token *t)
{
    struct a733_storage_native *n=p;
    const struct a733_usb_platform_config *b=&n->boot.usb;
    if(!a733_storage_native_live(n)||c!=&n->usb.config||c->owner!=b->owner||c->handoff!=b->handoff||
       c->deadline_us!=b->deadline_us||c->cold_reference!=b->cold_reference||
       memcmp(c->fabric,b->fabric,sizeof(c->fabric))||memcmp(c->rate_hz,b->rate_hz,sizeof(c->rate_hz))||
       t!=&n->usb.token||t->owner!=c->owner)return -1;
    return usb_providers(n);
}
static int usb_read(void *p,uintptr_t a,uint32_t *v)
{
    struct a733_storage_native *n=p;
    return a733_storage_native_live(n)&&xTaskGetCurrentTaskHandle()==n->task_handle[2]&&
        xSemaphoreGetMutexHolder(n->locks.mutex[0])==n->task_handle[2]&&
        usb_providers(n)==1&&
        a733_usb_platform_native_read(NULL,a,v);
}
static int usb_write(void *p,uintptr_t a,uint32_t v)
{
    struct a733_storage_native *n=p;
    return a733_storage_native_live(n)&&xTaskGetCurrentTaskHandle()==n->task_handle[2]&&
        xSemaphoreGetMutexHolder(n->locks.mutex[0])==n->task_handle[2]&&
        usb_providers(n)==1&&
        a733_usb_platform_native_write(NULL,a,v);
}
int a733_storage_native_usb_step(struct a733_storage_native *n)
{
    static const enum a733_rail rail[4]={A733_RAIL_DCDC1,A733_RAIL_DC1SW2,A733_RAIL_DCDC9,A733_RAIL_ELDO6};
    if(!a733_storage_native_live(n))return -1;
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
        enum a733_pmic_status r=a733_pmic_submit(&n->pmic,n->boot.usb.owner,&n->usb_rail,
            rail[n->usb_rail_index],n->boot.usb.deadline_us,&n->usb_request);
        if(r==A733_PMIC_BUSY)return 0;
        if(r!=A733_PMIC_PENDING)return -1;
        if(n->usb_request.id)n->usb_step=3;return 0;
    }
    if(n->usb_step==3) {
        struct a733_pmic_snapshot *s=&n->usb_snapshot[n->usb_rail_index];
        enum a733_pmic_status r=a733_pmic_result(&n->pmic,&n->usb_request,s);
        if(r==A733_PMIC_BUSY||r==A733_PMIC_PENDING)return 0;
        if(r!=A733_PMIC_COMPLETE||s->rail!=rail[n->usb_rail_index]||s->page_before||s->page_after||
           s->chip!=3||s->revision||s->decoded!=1||s->enabled!=1||
           s->programmed_uv!=n->boot.usb_supply_uv[n->usb_rail_index])return -1;
        n->usb_step=4;return 0;
    }
    if(n->usb_step==4) {
        enum a733_pmic_status r=a733_pmic_reap(&n->pmic,&n->usb_request);
        if(r==A733_PMIC_BUSY)return 0;if(r!=A733_PMIC_COMPLETE)return -1;
        n->usb_request=(struct a733_pmic_request){0};
        n->usb_step=++n->usb_rail_index==4?5:2;return 0;
    }
    enum a733_usb_platform_result r=a733_usb_platform_poll(&n->usb);
    if(r==A733_USB_PLATFORM_PENDING||r==A733_USB_PLATFORM_BUSY)return 0;
    if(r!=A733_USB_PLATFORM_PREPARED)return -1;
    atomic_store_explicit(&n->usb_prepared,1,memory_order_release);return 1;
}
int a733_storage_native_live(struct a733_storage_native *n)
{
    uint64_t time;
    return n&&atomic_load_explicit(&n->active,memory_order_acquire)&&
        !atomic_load_explicit(&n->failed,memory_order_acquire)&&
        !atomic_load_explicit(&n->channel.lost,memory_order_acquire)&&
        a733_maintenance_alive(n->maintenance)&&a733_counter_clock_now(&n->counter,&time)&&
        time<n->boot.grant.deadline_us;
}
int a733_storage_native_start(const struct a733_storage_native_boot *b,struct a733_maintenance_lease *maintenance,
    const struct a733_maintenance_transfer *transfer)
{
    struct a733_storage_native *n=&native;
    if(!b||!maintenance||!transfer||n->used||xTaskGetSchedulerState()!=taskSCHEDULER_NOT_STARTED)return 0;
    n->used=1;
    if(transfer->storage!=&b->grant||transfer->generation!=b->identity||memcmp(transfer->image,b->launch_artifact,32)||
       memcmp(transfer->qualification,b->grant.recovery,32)||
       !b->identity||!b->topology||!nonzero(b->topology_artifact,32)||!nonzero(b->launch_artifact,32)||
       b->mmc.handoff!=b->identity||b->mmc.topology!=b->topology||b->usb.handoff!=b->identity||
       b->pmic.identity!=b->identity||b->mmc.kind!=(enum sunxi_mmc_kind)b->grant.kind||
       b->mmc.emmc_host!=(enum a733_mmc_emmc_host)b->grant.host||
       b->mmc.lifetime_deadline_us!=b->grant.deadline_us||
       b->mmc.deadline_us>b->grant.deadline_us||b->usb.deadline_us>b->grant.deadline_us||
       b->pmic.startup_deadline>b->grant.deadline_us||
       !b->mmc.owner||!b->usb.owner||!b->pmic.owner||b->mmc.owner==b->usb.owner||
       b->mmc.owner==b->pmic.owner||b->usb.owner==b->pmic.owner)return 0;
    if(b->usb_supply_uv[0]!=3300000||b->usb_supply_uv[1]!=3300000||
       b->usb_supply_uv[2]<500000||b->usb_supply_uv[2]>1820000||
       b->usb_supply_uv[3]<500000||b->usb_supply_uv[3]>1500000||
       b->usb_supply_uv[2]<=b->usb_supply_uv[3])return 0;
    n->boot=*b;n->maintenance=maintenance;
    n->transfer=*transfer;n->transfer.storage=&n->boot.grant;
    atomic_init(&n->active,0);atomic_init(&n->failed,0);atomic_init(&n->pmic_ready,0);
    atomic_init(&n->service_ready,0);atomic_init(&n->usb_prepared,0);
    a733_storage_channel_init(&n->channel);
    n->transport.channel=&n->channel;n->transport.service=&n->service;
    atomic_init(&n->transport.version,0);atomic_init(&n->transport.generation,b->identity);
    atomic_init(&n->transport.accepted_sequence,0);atomic_init(&n->transport.accepted_command,0);
    atomic_init(&n->transport.response_sequence,0);atomic_init(&n->transport.completed_sequence,0);
    atomic_init(&n->transport.connected,0);atomic_init(&n->transport.lost,0);
    for(unsigned i=0;i<4;++i)atomic_init(&n->transport.accepted_hash[i],0);
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
    TaskFunction_t functions[3]={a733_storage_native_pmic_task,a733_storage_native_worker_task,a733_storage_native_usb_task};
    const char *names[3]={"storage-pmic","storage","storage-usb"};
    for(unsigned i=0;i<3;++i) {
        n->task_handle[i]=xTaskCreateStatic(functions[i],names[i],2048,n,4,n->stack[i],&n->task[i]);
        if(!n->task_handle[i])return 0;
    }
    n->transport.owner_task=n->task_handle[2];
    if(!a733_pmic_native_init(&n->pmic_native,&n->locks,n->task_handle[0])||
       !a733_pmic_init(&n->pmic,&n->boot.pmic,&n->pmic_native.operations,&n->pmic_native,&n->clocks,&n->rails,&n->pins)||
       !a733_gpu_resources_native_use_pmic(&n->gpu_native,&n->gpu,&n->pmic)||
       !a733_mmc_platform_native_init_selected(&n->mmc_native,&n->locks,n->task_handle[1],b->mmc.kind,b->mmc.emmc_host,mmc_admit,n)||
       !a733_mmc_platform_init(&n->mmc,&n->boot.mmc,&n->mmc_native.operations,&n->mmc_native,&n->clocks,&n->pins,&n->rails,&n->pmic))return 0;
    struct a733_usb_platform_ops usb_ops={now,usb_authorize,enter,leave,retain,usb_read,usb_write};
    if(!a733_usb_platform_init(&n->usb,&n->boot.usb,&usb_ops,n,&n->clocks))return 0;
    /* The real lease must authenticate this final tuple before publication. */
    if(!a733_maintenance_bind(maintenance,&n->transfer,&n->counter,&n->locks,&n->transport,
        &n->service,n->task_handle[1],n->task_handle[2]))return 0;
    atomic_store_explicit(&n->active,1,memory_order_release);
    if(!a733_storage_native_live(n)){a733_storage_native_fail(n);return 0;}
    return 1;
}
