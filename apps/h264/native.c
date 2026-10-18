#include "h264.h"
#include "a733_memory_uncached.h"
#include "a733_storage_digest.h"
#include "aarch64_device_memory.h"
#include "boot.h"
#include <string.h>
struct h264_application h264;
static uint64_t h264_deadline(void){return board.session.deadline_us-10000;}
static int h264_now(void *v,uint64_t *out)
{return v==&h264&&board_live(NULL)&&a733_counter_clock_now(&providers.shared.counter,out)==1;}
static int h264_enter(void *v,uint64_t d)
{return v==&h264&&board_live(NULL)?a733_platform_lock_enter(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK,d):-1;}
static int h264_leave(void *v)
{if(v!=&h264)return -1;a733_platform_lock_leave(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK);return 1;}
static int h264_device(uintptr_t address)
{
    struct aarch64_translation t;
    return aarch64_device_translate(address,1,&t)&&t.physical==address&&t.attributes==0&&t.shareability==2;
}
static int h264_load(uintptr_t address,uint32_t *value)
{if(!h264_device(address)||!board_live(NULL))return -1;__asm__ volatile("dsb sy":::"memory");*value=*(volatile uint32_t *)address;__asm__ volatile("dsb sy":::"memory");return board_live(NULL)?1:-1;}
static int h264_iommu_read(void *v,uint64_t token,uint32_t off,uint32_t *value,uint64_t d)
{return v==&h264&&token==h264.session.authority&&d==h264_deadline()&&off<0x20000&&!(off&3)?h264_load(A733_IOMMU_BASE+off,value):-1;}
static int h264_iommu_write(void *v,uint64_t token,uint32_t off,uint32_t value,uint64_t d)
{
    uint32_t ignored;
    if(h264_iommu_read(v,token,off,&ignored,d)!=1)return -1;
    if(h264_enter(v,d)!=1)return -1;
    *(volatile uint32_t *)(uintptr_t)(A733_IOMMU_BASE+off)=value;
    __asm__ volatile("dsb sy":::"memory");
    if(!board_live(NULL))return -1;return h264_leave(v);
}
static int h264_publish(void *v,uint64_t token,const void *p,size_t n,uint64_t d)
{
    if(v!=&h264||token!=h264.session.authority||p!=h264.table||n!=32768||d!=h264_deadline())return -1;
    __asm__ volatile("dsb sy":::"memory");return board_live(NULL)?1:-1;
}
static int h264_iommu_admit(void *v,const struct a733_iommu_tables *t,uint64_t d,struct a733_iommu_admission *a)
{
    if(v!=&h264||t->words!=h264.table||d!=h264_deadline()||!board_live(NULL))return -1;
    *a=(struct a733_iommu_admission){.token=h264.session.authority,
        .excluded_masters=A733_IOMMU_REAL_MASTERS,.powered_masters=0xc0,
        .inherited_resolved=1,.transactions_drained=1,.clocks_held=1,.retained_power_removal=1};
    /* These facts come from the full authenticated initial-isolation record,
     * never inferred from disabled ENABLE, empty fault registers or CPU fences. */
    return a733_clock_token_status(&providers.shared.clocks,&h264.iommu_clock)==A733_CLOCK_OK?1:-1;
}
static int h264_authority(void *v,const struct a733_video_platform_config *c,
    const struct a733_h264_job *j,uint64_t d,uint64_t *token)
{
    if(v!=&h264||!token)return -1;
    if(*token&&*token!=h264.session.authority)return -1;*token=h264.session.authority;
    if(!board_live(NULL)||d!=h264_deadline()||memcmp(c,&h264.config,sizeof(*c))||
       j->input!=h264.input)return -1;
    for(unsigned i=0;i<A733_H264_BUFFERS;++i)if(j->buffers[i].iova!=h264.job.buffers[i].iova||
        j->buffers[i].bytes!=h264.job.buffers[i].bytes)return -1;
    uint8_t hash[32];storage_digest_bytes(j->input,A733_H264_SLICE_BYTES,hash);
    if(memcmp(hash,h264.session.fixture,32))return -1;
    for(uintptr_t p=0x17000;p<0x22000;p+=4096)if(!h264_device(p))return -1;
    for(uintptr_t p=0x01c0e000;p<0x01c0f000;p+=4096)if(!h264_device(p))return -1;
    uint32_t gate;
    for(unsigned i=0;i<2;++i)if(h264_load(0x02002000+(i?0x5b4:0x58c),&gate)!=1||
       (gate&0x10007)!=h264.session.iommu_gates[i])return -1;
    return 1;
}
static int h264_memory(void *v,uint64_t token,const struct a733_h264_job *j,uint64_t d)
{
    (void)j;
    if(v!=&h264||token!=h264.session.authority||d!=h264_deadline())return -1;
    if(!h264.iommu.initialized){
        struct a733_iommu_ops ops={h264_now,h264_iommu_admit,h264_iommu_read,h264_iommu_write,h264_publish};
        if(a733_iommu_init_decoder(&h264.iommu,&ops,&h264,&h264.tables)!=1||a733_iommu_open(&h264.iommu,d)!=1)return -1;
    }
    return a733_iommu_poll(&h264.iommu);
}
static const uint32_t capacity[]={4096,4096,4096,0x21000,4096,0xc000};
static const uint32_t used[]={4096,2048,1024,A733_H264_PICTURE_INFO_BYTES,2048,0xc000};
static const uint32_t prefix[]={4096,4096,4096,4096,4096,16384};
static int h264_guards(void)
{
    for(unsigned a=0;a<A733_H264_BUFFERS;++a){
        struct device_memory_extent e;enum device_memory_state state;
        if(!device_memory_describe(h264.pool,h264.leases[a+1],&e,&state)||state!=DEVICE_MEMORY_DEVICE)return 0;
        const volatile uint8_t *p=(const void *)e.cpu;
        if(e.bytes!=prefix[a]+capacity[a]+4096||e.cpu+prefix[a]!=(uintptr_t)h264.buffers[a])return 0;
        for(unsigned i=0;i<prefix[a];++i)if(p[i]!=0xa5)return 0;
        for(unsigned i=prefix[a]+used[a];i<e.bytes;++i)if(p[i]!=0xa5)return 0;
    }
    return 1;
}
static int h264_copied(void *v,uint64_t token,uint64_t d)
{
    if(v!=&h264||token!=h264.session.authority||d!=h264_deadline()||!board_live(NULL))return -1;
    if(!h264.copy_step){if(a733_iommu_scan(&h264.iommu,d)!=1)return -1;h264.copy_step=1;}
    int r=a733_iommu_poll(&h264.iommu);if(r!=1)return r;
    if(h264.iommu.fault.scanned_masters!=0xc0||!h264_guards())return -1;
    if(h264.copy_step==2){
        uint32_t final_status;
        if(h264_load(0x01c0e228,&final_status)!=1||
           (final_status&(A733_H264_BUSY_MASK|0xe))||!h264_guards())return -1;
        storage_digest_bytes(h264.payload,2304,h264.digest);return 1;
    }
    uint32_t status;
    if(h264_load(0x01c0e228,&status)!=1||(status&(A733_H264_BUSY_MASK|0xe)))return -1;
    __asm__ volatile("dsb sy":::"memory");
    for(unsigned row=0;row<32;++row)for(unsigned col=0;col<48;++col)h264.payload[row*48+col]=((volatile uint8_t *)h264.luma)[row*64+col];
    for(unsigned plane=0;plane<2;++plane)for(unsigned row=0;row<16;++row)for(unsigned col=0;col<24;++col)
        h264.payload[1536+plane*384+row*24+col]=((volatile uint8_t *)h264.chroma)[plane*512+row*32+col];
    __asm__ volatile("dsb sy":::"memory");
    if(!h264_guards()||memcmp(h264.payload,h264_reference,2304)||!board_live(NULL))return -1;
    if(a733_iommu_scan(&h264.iommu,d)!=1)return -1;
    h264.copy_step=2;return 0;
}
static int h264_allocate(void)
{
    h264.pool=a733_memory_uncached_init();if(!h264.pool)return 0;
    for(unsigned i=0;i<1+A733_H264_BUFFERS;++i){
        struct device_memory_extent e;
        uint64_t bytes=i?prefix[i-1]+capacity[i-1]+4096:32768;
        uint64_t alignment=!i||i==1+A733_H264_NEIGHBOR?16384:4096;
        if(!device_memory_acquire_aligned(h264.pool,h264.session.owner,bytes,alignment,&h264.leases[i],&e))return 0;
        if(i==0)h264.table=(void *)e.cpu;
        else{memset((void *)e.cpu,0xa5,e.bytes);h264.buffers[i-1]=(void *)(e.cpu+prefix[i-1]);
            memset(h264.buffers[i-1],0,used[i-1]);}
    }
    h264.input=h264.buffers[A733_H264_INPUT];h264.luma=h264.buffers[A733_H264_Y];h264.chroma=h264.buffers[A733_H264_C];
    memcpy(h264.input,h264_fixture,A733_H264_SLICE_BYTES);
    static const uint32_t iova[]={0x10001000,0x10010000,0x10014000,0x10020000,0x10044000,0x10048000};
    struct a733_iommu_span spans[A733_H264_BUFFERS];h264.job.input=h264.input;
    for(unsigned i=0;i<A733_H264_BUFFERS;++i){
        h264.job.buffers[i]=(struct a733_h264_span){iova[i],capacity[i]};
        spans[i]=(struct a733_iommu_span){iova[i],(uintptr_t)h264.buffers[i],capacity[i],
            i==A733_H264_INPUT?A733_IOMMU_READ:i<=A733_H264_C?A733_IOMMU_WRITE:A733_IOMMU_READ|A733_IOMMU_WRITE};
    }
    if(a733_iommu_build(&h264.tables,h264.table,(uintptr_t)h264.table,spans,A733_H264_BUFFERS)!=1)return 0;
    __asm__ volatile("dsb sy":::"memory");
    for(unsigned i=0;i<1+A733_H264_BUFFERS;++i){
        struct device_memory_allocation *a=device_memory_lookup(h264.pool,h264.leases[i]);
        if(!a||a->state!=DEVICE_MEMORY_CPU)return 0;
        a->state=DEVICE_MEMORY_DEVICE;
    }
    return 1;
}
static int h264_setup(void)
{
    struct a733_video_platform_config *c=&h264.config;
    if(!c->owner)*c=(struct a733_video_platform_config){.clocks=&providers.shared.clocks,.power=&providers.shared.gpu,
        .power_client=(unsigned)h264.session.pck_client,.owner=h264.session.owner,.handoff=h264.session.board.generation,
        .module=h264.session.module|0x80000000,.module_hz=h264.session.module_hz,.start=A733_VIDEO_COLD_NS_ROM};
    memcpy(c->fabric,h264.session.board.fabric,sizeof(c->fabric));memcpy(c->fabric_hz,h264.session.board.fabric_hz,sizeof(c->fabric_hz));
    enum a733_rail_result rr;
    if(!h264.rail.id)rr=a733_rail_acquire(&providers.shared.rails,c->owner,
        A733_RAIL_BIT(h264.session.supply_rail),0,h264_deadline(),&h264.rail);
    else rr=a733_rail_poll(&providers.shared.rails,&h264.rail);
    if(rr==A733_RAIL_BUSY||rr==A733_RAIL_PENDING)return 0;
    if(rr!=A733_RAIL_READY)return -1;
    enum a733_pmic_status pr;
    if(!h264.request.id){
        pr=a733_pmic_submit(&providers.shared.pmic,c->owner,&h264.rail,
            (enum a733_rail)h264.session.supply_rail,h264_deadline(),&h264.request);
        return pr==A733_PMIC_PENDING||pr==A733_PMIC_BUSY?0:-1;
    }
    pr=a733_pmic_result(&providers.shared.pmic,&h264.request,&h264.supply);
    if(pr==A733_PMIC_PENDING||pr==A733_PMIC_BUSY)return 0;
    if(pr!=A733_PMIC_COMPLETE||h264.supply.rail!=h264.session.supply_rail||
       h264.supply.page_before||h264.supply.page_after||h264.supply.chip!=3||h264.supply.revision||
       !h264.supply.enabled||!h264.supply.decoded||h264.supply.programmed_uv!=h264.session.supply_uv)return -1;
    a733_clock_mask_t scopes;if(!a733_video_platform_scopes(c,&scopes))return -1;
    if(!c->clock.id){enum a733_clock_result r=a733_clock_acquire(c->clocks,c->owner,scopes,A733_CLOCK_VE_DEC,h264_deadline(),&c->clock);if(r==A733_CLOCK_BUSY)return 0;if(r!=A733_CLOCK_OK)return -1;}
    if(!h264.iommu_clock.id){enum a733_clock_result r=a733_clock_acquire(c->clocks,c->owner,A733_CLOCK_FABRIC|A733_CLOCK_REF|A733_CLOCK_OSC,
        A733_CLOCK_IOMMU_SYS|A733_CLOCK_VI|A733_CLOCK_DE_SYS,h264_deadline(),&h264.iommu_clock);if(r==A733_CLOCK_BUSY)return 0;if(r!=A733_CLOCK_OK)return -1;}
    if(!h264.power_token){enum a733_gpu_resource_result r=a733_gpu_resources_acquire(c->power,c->power_client,A733_GPU_RESOURCE_OSC|A733_GPU_RESOURCE_PCK,h264_deadline(),&h264.power_token,&h264.power_ready);if(r==A733_GPU_RESOURCE_BUSY)return 0;if(r!=A733_GPU_RESOURCE_OK)return -1;}
    c->pck=(struct a733_gpu_resource_lease){h264.power_token,A733_GPU_RESOURCE_OSC|A733_GPU_RESOURCE_PCK,A733_GPU_RESOURCE_STABLE};
    struct a733_h264_platform_ops ops={h264_now,h264_authority,h264_memory,h264_copied,
        h264_enter,h264_leave,a733_video_platform_native_read,a733_video_platform_native_write};
    if(!h264_allocate()||!a733_h264_platform_init(&h264.platform,c,&ops,&h264))return -1;
    return a733_h264_begin(&h264.run,a733_video_platform_h264_ops(),&h264.platform,&h264.job,h264_deadline())?1:-1;
}
void h264_task(void *unused)
{
    (void)unused;
    for(;;){
        if(!board_live(NULL)){atomic_store_explicit(&h264.result,2,memory_order_release);vTaskDelay(1);continue;}
        if(atomic_load_explicit(&providers.shared.pmic_ready,memory_order_acquire)&&!atomic_load_explicit(&h264.result,memory_order_acquire)){
            int r=h264.stage?a733_h264_poll(&h264.run):h264_setup();
            if(r<0)atomic_store_explicit(&h264.result,2,memory_order_release);
            else if(!h264.stage&&r==1)h264.stage=1;
            else if(h264.stage&&r==1&&h264.run.state==A733_H264_COPIED)atomic_store_explicit(&h264.result,1,memory_order_release);
        }
        vTaskDelay(1);
    }
}
