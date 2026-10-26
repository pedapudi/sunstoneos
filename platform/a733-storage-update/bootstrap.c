#define A733_STORAGE_NATIVE_IMPLEMENTATION
#include "a733_storage_update_native.h"
#include "a733_usb_platform_native.h"
#include "task.h"
#include "aarch64_device_memory.h"
#include <string.h>

static struct a733_storage_native native;
extern unsigned char __uncached_pool_start[],__uncached_pool_end[];
static int dma_span(uintptr_t address,size_t bytes,unsigned attributes)
{
    uintptr_t last=(address+bytes-1)&~(uintptr_t)4095;
    for(uintptr_t page=address&~(uintptr_t)4095;;page+=4096) {
        struct aarch64_translation t;
        if(!aarch64_device_translate(page,1,&t)||t.physical!=page||
           t.attributes!=attributes||t.shareability!=3)return 0;
        if(page==last)return 1;
    }
}
/* The worker owns the complete cache-line allocation until DONE/take. The
 * permanent service never shares its input or state lines with this buffer. */
static int dma_sync(void *cookie,uint8_t *buffer,size_t bytes,unsigned phase)
{
    struct a733_storage_native *n=&native;
    return cookie==&n->mmc_native&&n->mmc.config.dma==
        (volatile struct sunxi_mmc_dma_descriptor *)__uncached_pool_start&&
        buffer==n->service.readback&&bytes&&bytes<=sizeof(n->service.readback)&&!(bytes%512)&&
        (phase==SUNXI_MMC_DMA_BEFORE||phase==SUNXI_MMC_DMA_AFTER)&&a733_storage_native_live(n)&&
        aarch64_device_cache(phase==SUNXI_MMC_DMA_BEFORE?AARCH64_CACHE_CLEAN:AARCH64_CACHE_INVALIDATE,
            (uintptr_t)buffer,bytes)==1;
}
int a733_storage_native_task_privileged(void (*entry)(void *),void *context)
{
    return native.used&&context==&native&&(entry==a733_storage_native_pmic_task||
        entry==a733_storage_native_worker_task||entry==a733_storage_native_usb_task);
}
static int nonzero(const uint8_t *p,size_t n)
{unsigned v=0;for(size_t i=0;i<n;++i)v|=p[i];return v!=0;}
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
       c->dma!=b->dma||c->kind!=b->kind||c->emmc_host!=b->emmc_host||c->sd_profile!=b->sd_profile||c->voltage_mask!=b->voltage_mask||
       memcmp(c->fabric,b->fabric,sizeof(c->fabric))||memcmp(c->fabric_hz,b->fabric_hz,sizeof(c->fabric_hz))||
       memcmp(c->expected,b->expected,sizeof(c->expected))||memcmp(c->desired,b->desired,sizeof(c->desired))||
       t->owner!=c->owner)return -1;
    enum a733_clock_result r=a733_clock_token_status(&n->providers.clocks,t);
    if(r==A733_CLOCK_BUSY)return 0;
    if(r!=A733_CLOCK_OK)return -1;
    if(c->dma&&(c->dma!=(volatile struct sunxi_mmc_dma_descriptor *)__uncached_pool_start||
       (uintptr_t)__uncached_pool_start!=UINT32_C(0x41c00000)||
       (uintptr_t)__uncached_pool_end!=UINT32_C(0x41e00000)||
       !dma_span((uintptr_t)c->dma,SUNXI_MMC_DMA_DESCRIPTORS*sizeof(*c->dma),0x44)||
       !dma_span((uintptr_t)n->service.readback,sizeof(n->service.readback),0xff)))return -1;
    return 1;
}
int a733_storage_native_usb_health(struct a733_storage_native *n)
{return n?a733_native_usb_health(&n->providers):-1;}
int a733_storage_native_usb_step(struct a733_storage_native *n)
{return n?a733_native_usb_step(&n->providers):-1;}
static int providers_live(void *p){return a733_storage_native_live(p);}
static void providers_fail(void *p){a733_storage_native_fail(p);}
int a733_storage_native_live(struct a733_storage_native *n)
{
    uint64_t time;
    return n&&atomic_load_explicit(&n->active,memory_order_acquire)&&
        !atomic_load_explicit(&n->failed,memory_order_acquire)&&
        !atomic_load_explicit(&n->channel.lost,memory_order_acquire)&&
        a733_maintenance_alive(n->maintenance)&&a733_counter_clock_now(&n->providers.counter,&time)&&
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
       b->mmc.dma||!b->mmc.owner||!b->usb.owner||!b->pmic.owner||b->mmc.owner==b->usb.owner||
       b->mmc.owner==b->pmic.owner||b->usb.owner==b->pmic.owner)return 0;
    if(b->usb_supply_uv[0]!=3300000||b->usb_supply_uv[1]!=3300000||
       b->usb_supply_uv[2]<500000||b->usb_supply_uv[2]>1820000||
       b->usb_supply_uv[3]<500000||b->usb_supply_uv[3]>1500000||
       b->usb_supply_uv[2]<=b->usb_supply_uv[3])return 0;
    n->boot=*b;n->maintenance=maintenance;
    if(b->mmc.kind==SUNXI_MMC_SD&&b->mmc.sd_profile==SUNXI_MMC_SD_HIGH_SPEED4)
        n->boot.mmc.dma=(volatile struct sunxi_mmc_dma_descriptor *)__uncached_pool_start;
    n->transfer=*transfer;n->transfer.storage=&n->boot.grant;
    atomic_init(&n->active,0);atomic_init(&n->failed,0);
    atomic_init(&n->service_ready,0);
    a733_storage_channel_init(&n->channel);
    n->transport.channel=&n->channel;n->transport.service=&n->service;
    atomic_init(&n->transport.version,0);atomic_init(&n->transport.generation,b->identity);
    atomic_init(&n->transport.accepted_sequence,0);atomic_init(&n->transport.accepted_command,0);
    atomic_init(&n->transport.response_sequence,0);atomic_init(&n->transport.completed_sequence,0);
    atomic_init(&n->transport.connected,0);atomic_init(&n->transport.lost,0);
    for(unsigned i=0;i<4;++i)atomic_init(&n->transport.accepted_hash[i],0);
    struct a733_native_usb_config providers={.identity=b->identity,.pmic=b->pmic,.pins=b->pins,.usb=b->usb};
    memcpy(providers.usb_supply_uv,b->usb_supply_uv,sizeof(providers.usb_supply_uv));
    if(!a733_native_usb_init(&n->providers,&providers,providers_live,providers_fail,n))return 0;
    TaskFunction_t functions[3]={a733_storage_native_pmic_task,a733_storage_native_worker_task,a733_storage_native_usb_task};
    const char *names[3]={"storage-pmic","storage","storage-usb"};
    for(unsigned i=0;i<3;++i) {
        n->task_handle[i]=xTaskCreateStatic(functions[i],names[i],2048,n,4,n->stack[i],&n->task[i]);
        if(!n->task_handle[i])return 0;
    }
    n->transport.owner_task=n->task_handle[2];
    if(!a733_native_usb_bind(&n->providers,n->task_handle[0],n->task_handle[2])||
       !a733_mmc_platform_native_init_selected(&n->mmc_native,&n->providers.locks,n->task_handle[1],b->mmc.kind,b->mmc.emmc_host,mmc_admit,n))return 0;
    if(n->boot.mmc.dma)n->mmc_native.operations.dma_sync=dma_sync;
    if(!a733_mmc_platform_init(&n->mmc,&n->boot.mmc,&n->mmc_native.operations,&n->mmc_native,&n->providers.clocks,&n->providers.pins,&n->providers.rails,&n->providers.pmic))return 0;
    /* The real lease must authenticate this final tuple before publication. */
    if(!a733_maintenance_bind(maintenance,&n->transfer,&n->providers.counter,&n->providers.locks,&n->transport,
        &n->service,n->task_handle[1],n->task_handle[2]))return 0;
    atomic_store_explicit(&n->active,1,memory_order_release);
    if(!a733_storage_native_live(n)){a733_storage_native_fail(n);return 0;}
    return 1;
}
