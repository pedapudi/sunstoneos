#include "jpeg.h"
#include "a733_memory_uncached.h"
#include "a733_storage_digest.h"
#include "aarch64_device_memory.h"
#include "boot.h"
#include <string.h>
struct jpeg_application jpeg;
static uint64_t jpeg_deadline(void){return board.session.deadline_us-10000;}
static int jpeg_now(void *v,uint64_t *out)
{return v==&jpeg&&board_live(NULL)&&a733_counter_clock_now(&providers.shared.counter,out)==1;}
static int jpeg_enter(void *v,uint64_t d)
{return v==&jpeg&&board_live(NULL)?a733_platform_lock_enter(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK,d):-1;}
static int jpeg_leave(void *v)
{if(v!=&jpeg)return -1;a733_platform_lock_leave(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK);return 1;}
static int jpeg_device(uintptr_t address)
{
    struct aarch64_translation t;
    return aarch64_device_translate(address,1,&t)&&t.physical==address&&t.attributes==0&&t.shareability==2;
}
static int jpeg_load(uintptr_t address,uint32_t *value)
{if(!jpeg_device(address)||!board_live(NULL))return -1;__asm__ volatile("dsb sy":::"memory");*value=*(volatile uint32_t *)address;__asm__ volatile("dsb sy":::"memory");return board_live(NULL)?1:-1;}
static int jpeg_iommu_read(void *v,uint64_t token,uint32_t off,uint32_t *value,uint64_t d)
{return v==&jpeg&&token==jpeg.session.authority&&d==jpeg_deadline()&&off<0x20000&&!(off&3)?jpeg_load(A733_IOMMU_BASE+off,value):-1;}
static int jpeg_iommu_write(void *v,uint64_t token,uint32_t off,uint32_t value,uint64_t d)
{
    uint32_t ignored;
    if(jpeg_iommu_read(v,token,off,&ignored,d)!=1)return -1;
    if(jpeg_enter(v,d)!=1)return -1;
    *(volatile uint32_t *)(uintptr_t)(A733_IOMMU_BASE+off)=value;
    __asm__ volatile("dsb sy":::"memory");
    if(!board_live(NULL))return -1;return jpeg_leave(v);
}
static int jpeg_publish(void *v,uint64_t token,const void *p,size_t n,uint64_t d)
{
    if(v!=&jpeg||token!=jpeg.session.authority||p!=jpeg.table||n!=32768||d!=jpeg_deadline())return -1;
    __asm__ volatile("dsb sy":::"memory");return board_live(NULL)?1:-1;
}
static int jpeg_iommu_admit(void *v,const struct a733_iommu_tables *t,uint64_t d,struct a733_iommu_admission *a)
{
    if(v!=&jpeg||t->words!=jpeg.table||d!=jpeg_deadline()||!board_live(NULL))return -1;
    *a=(struct a733_iommu_admission){.token=jpeg.session.authority,
        .excluded_masters=A733_IOMMU_REAL_MASTERS,.powered_masters=0xc0,
        .inherited_resolved=1,.transactions_drained=1,.clocks_held=1,.retained_power_removal=1};
    /* These facts come from the full authenticated initial-isolation record,
     * never inferred from disabled ENABLE, empty fault registers or CPU fences. */
    return a733_clock_token_status(&providers.shared.clocks,&jpeg.iommu_clock)==A733_CLOCK_OK?1:-1;
}
static int jpeg_authority(void *v,const struct a733_video_platform_config *c,
    const struct a733_video_jpeg_job *j,uint64_t d,uint64_t *token)
{
    if(v!=&jpeg||!token)return -1;
    if(*token&&*token!=jpeg.session.authority)return -1;*token=jpeg.session.authority;
    if(!board_live(NULL)||d!=jpeg_deadline()||memcmp(c,&jpeg.config,sizeof(*c))||
       j->input!=jpeg.input||j->file_bytes!=jpeg.supplied.input_bytes||j->input_bytes!=4096||
       j->input_iova!=0x10001000||j->luma_iova!=0x10005000||j->chroma_iova!=0x10009000)return -1;
    uint8_t hash[32];storage_digest_bytes(j->input,jpeg.supplied.input_bytes,hash);
    if(memcmp(hash,jpeg.session.fixture,32))return -1;
    for(uintptr_t p=0x17000;p<0x22000;p+=4096)if(!jpeg_device(p))return -1;
    for(uintptr_t p=0x01c0e000;p<0x01c0f000;p+=4096)if(!jpeg_device(p))return -1;
    uint32_t gate;
    for(unsigned i=0;i<2;++i)if(jpeg_load(0x02002000+(i?0x5b4:0x58c),&gate)!=1||
       (gate&0x10007)!=jpeg.session.iommu_gates[i])return -1;
    return 1;
}
static int jpeg_memory(void *v,uint64_t token,const struct a733_video_jpeg_job *j,
    const struct a733_video_jpeg *pic,uint32_t stride,uint32_t rows,uint64_t d)
{
    (void)j;
    if(v!=&jpeg||token!=jpeg.session.authority||d!=jpeg_deadline()||!jpeg_input_ready(&jpeg.supplied)||pic->width!=jpeg.supplied.width||pic->height!=jpeg.supplied.height||
       stride!=jpeg.supplied.stride||rows!=jpeg.supplied.rows)return -1;
    if(!jpeg.iommu.initialized){
        struct a733_iommu_ops ops={jpeg_now,jpeg_iommu_admit,jpeg_iommu_read,jpeg_iommu_write,jpeg_publish};
        if(a733_iommu_init_decoder(&jpeg.iommu,&ops,&jpeg,&jpeg.tables)!=1||a733_iommu_open(&jpeg.iommu,d)!=1)return -1;
    }
    return a733_iommu_poll(&jpeg.iommu);
}
static int jpeg_no_drain(void *v,uint64_t t,uint64_t d)
{(void)v;(void)t;(void)d;return -1;}
static int jpeg_guards(void)
{
    for(unsigned a=0;a<3;++a){
        struct device_memory_extent e;enum device_memory_state state;
        if(!device_memory_describe(jpeg.pool,jpeg.leases[a+1],&e,&state)||state!=DEVICE_MEMORY_DEVICE)return 0;
        const volatile uint8_t *p=(const void *)e.cpu;
        for(unsigned i=0;i<4096;++i)if(p[i]!=0xa5||p[8192+i]!=0xa5)return 0;
        unsigned used=a==0?4096:a==1?jpeg.supplied.luma_bytes:jpeg.supplied.chroma_bytes;
        for(unsigned i=used;i<4096;++i)if(p[4096+i]!=0xa5)return 0;
    }
    return 1;
}
static int jpeg_copied(void *v,uint64_t token,uint64_t d)
{
    if(v!=&jpeg||token!=jpeg.session.authority||d!=jpeg_deadline()||!board_live(NULL))return -1;
    if(!jpeg.copy_step){if(a733_iommu_scan(&jpeg.iommu,d)!=1)return -1;jpeg.copy_step=1;}
    int r=a733_iommu_poll(&jpeg.iommu);if(r!=1)return r;
    if(jpeg.iommu.fault.scanned_masters!=0xc0||!jpeg_guards())return -1;
    if(jpeg.copy_step==2){
        uint32_t final_status;
        if(jpeg_load(0x01c0e11c,&final_status)!=1||(final_status&0x0018000e)||!jpeg_guards())return -1;
        storage_digest_bytes(jpeg.payload,jpeg.supplied.output_bytes,jpeg.digest);return 1;
    }
    uint32_t status;
    if(jpeg_load(0x01c0e11c,&status)!=1||(status&0x0018000e))return -1;
    __asm__ volatile("dsb sy":::"memory");
    uint32_t w=jpeg.supplied.width,h=jpeg.supplied.height,stride=jpeg.supplied.stride;
    uint32_t cw=(w+1u)/2u,ch=(h+1u)/2u,plane_bytes=jpeg.supplied.luma_bytes/4u;
    for(unsigned row=0;row<h;++row)for(unsigned col=0;col<w;++col)
        jpeg.payload[row*w+col]=((volatile uint8_t *)jpeg.luma)[row*stride+col];
    for(unsigned plane=0;plane<2;++plane)for(unsigned row=0;row<ch;++row)for(unsigned col=0;col<cw;++col)
        jpeg.payload[w*h+plane*cw*ch+row*cw+col]=((volatile uint8_t *)jpeg.chroma)[plane*plane_bytes+row*(stride/2u)+col];
    __asm__ volatile("dsb sy":::"memory");
    if(!jpeg_guards()||!board_live(NULL)||
       (jpeg.supplied.oracle_bytes&&memcmp(jpeg.payload,jpeg.supplied.oracle,jpeg.supplied.output_bytes)))return -1;
    if(a733_iommu_scan(&jpeg.iommu,d)!=1)return -1;
    jpeg.copy_step=2;return 0;
}
static int jpeg_retained(void *v,uint64_t t,uint64_t d)
{return a733_video_platform_retained_copy(v,t,d,jpeg_copied,&jpeg);}
static int jpeg_allocate(void)
{
    jpeg.pool=a733_memory_uncached_init();if(!jpeg.pool)return 0;
    for(unsigned i=0;i<4;++i){
        struct device_memory_extent e;
        if(!device_memory_acquire_aligned(jpeg.pool,jpeg.session.owner,i?12288:32768,i?4096:16384,&jpeg.leases[i],&e))return 0;
        if(i==0)jpeg.table=(void *)e.cpu;
        else{memset((void *)e.cpu,0xa5,e.bytes);uint8_t *p=(void *)(e.cpu+4096);if(i==1)jpeg.input=p;else if(i==2)jpeg.luma=p;else jpeg.chroma=p;}
    }
    memset(jpeg.input,0,4096);memcpy(jpeg.input,jpeg.supplied.bytes,jpeg.supplied.input_bytes);
    struct a733_iommu_span spans[]={{0x10001000,(uintptr_t)jpeg.input,4096,A733_IOMMU_READ},
        {0x10005000,(uintptr_t)jpeg.luma,4096,A733_IOMMU_WRITE},{0x10009000,(uintptr_t)jpeg.chroma,4096,A733_IOMMU_WRITE}};
    if(a733_iommu_build(&jpeg.tables,jpeg.table,(uintptr_t)jpeg.table,spans,3)!=1)return 0;
    __asm__ volatile("dsb sy":::"memory");
    for(unsigned i=0;i<4;++i){
        struct device_memory_allocation *a=device_memory_lookup(jpeg.pool,jpeg.leases[i]);
        if(!a||a->state!=DEVICE_MEMORY_CPU)return 0;
        a->state=DEVICE_MEMORY_DEVICE;
    }
    return 1;
}
static int jpeg_setup(void)
{
    struct a733_video_platform_config *c=&jpeg.config;
    if(!c->owner)*c=(struct a733_video_platform_config){.clocks=&providers.shared.clocks,.power=&providers.shared.gpu,
        .power_client=(unsigned)jpeg.session.pck_client,.owner=jpeg.session.owner,.handoff=jpeg.session.board.generation,
        .module=jpeg.session.module|0x80000000,.module_hz=jpeg.session.module_hz,.start=A733_VIDEO_COLD_NS_ROM};
    memcpy(c->fabric,jpeg.session.board.fabric,sizeof(c->fabric));memcpy(c->fabric_hz,jpeg.session.board.fabric_hz,sizeof(c->fabric_hz));
    enum a733_rail_result rr;
    if(!jpeg.rail.id)rr=a733_rail_acquire(&providers.shared.rails,c->owner,
        A733_RAIL_BIT(jpeg.session.supply_rail),0,jpeg_deadline(),&jpeg.rail);
    else rr=a733_rail_poll(&providers.shared.rails,&jpeg.rail);
    if(rr==A733_RAIL_BUSY||rr==A733_RAIL_PENDING)return 0;
    if(rr!=A733_RAIL_READY)return -1;
    enum a733_pmic_status pr;
    if(!jpeg.request.id){
        pr=a733_pmic_submit(&providers.shared.pmic,c->owner,&jpeg.rail,
            (enum a733_rail)jpeg.session.supply_rail,jpeg_deadline(),&jpeg.request);
        return pr==A733_PMIC_PENDING||pr==A733_PMIC_BUSY?0:-1;
    }
    pr=a733_pmic_result(&providers.shared.pmic,&jpeg.request,&jpeg.supply);
    if(pr==A733_PMIC_PENDING||pr==A733_PMIC_BUSY)return 0;
    if(pr!=A733_PMIC_COMPLETE||jpeg.supply.rail!=jpeg.session.supply_rail||
       jpeg.supply.page_before||jpeg.supply.page_after||jpeg.supply.chip!=3||jpeg.supply.revision||
       !jpeg.supply.enabled||!jpeg.supply.decoded||jpeg.supply.programmed_uv!=jpeg.session.supply_uv)return -1;
    a733_clock_mask_t scopes;if(!a733_video_platform_scopes(c,&scopes))return -1;
    if(!c->clock.id){enum a733_clock_result r=a733_clock_acquire(c->clocks,c->owner,scopes,A733_CLOCK_VE_DEC,jpeg_deadline(),&c->clock);if(r==A733_CLOCK_BUSY)return 0;if(r!=A733_CLOCK_OK)return -1;}
    if(!jpeg.iommu_clock.id){enum a733_clock_result r=a733_clock_acquire(c->clocks,c->owner,A733_CLOCK_FABRIC|A733_CLOCK_REF|A733_CLOCK_OSC,
        A733_CLOCK_IOMMU_SYS|A733_CLOCK_VI|A733_CLOCK_DE_SYS|A733_CLOCK_VE_ENC,jpeg_deadline(),&jpeg.iommu_clock);if(r==A733_CLOCK_BUSY)return 0;if(r!=A733_CLOCK_OK)return -1;}
    if(!jpeg.power_token){enum a733_gpu_resource_result r=a733_gpu_resources_acquire(c->power,c->power_client,A733_GPU_RESOURCE_OSC|A733_GPU_RESOURCE_PCK,jpeg_deadline(),&jpeg.power_token,&jpeg.power_ready);if(r==A733_GPU_RESOURCE_BUSY)return 0;if(r!=A733_GPU_RESOURCE_OK)return -1;}
    c->pck=(struct a733_gpu_resource_lease){jpeg.power_token,A733_GPU_RESOURCE_OSC|A733_GPU_RESOURCE_PCK,A733_GPU_RESOURCE_STABLE};
    struct a733_video_platform_ops ops={jpeg_now,jpeg_authority,jpeg_memory,jpeg_no_drain,jpeg_enter,jpeg_leave,a733_video_platform_native_read,a733_video_platform_native_write};
    if(!jpeg_allocate()||!a733_video_platform_init(&jpeg.platform,c,&ops,&jpeg))return -1;
    struct a733_video_jpeg_job job={jpeg.input,(uint32_t)jpeg.supplied.input_bytes,0x10001000,4096,0x10005000,4096,0x10009000,4096};
    return a733_video_jpeg_begin_retained(&jpeg.run,a733_video_platform_jpeg_ops(),&jpeg.platform,&job,jpeg_deadline(),jpeg_retained)?1:-1;
}
/* The controller performs at most one register operation per poll. Repeat
 * polls while each one advances its state or index, so programming sequences
 * finish in one wake. Waiting for hardware, a copy step or a busy resource
 * makes no progress and sleeps a tick; the budget bounds one wake's work. */
#define JPEG_POLL_BUDGET 1024u
static int jpeg_advance(void)
{
    for(unsigned n=0;;){
        enum a733_video_jpeg_state state=jpeg.run.state;unsigned index=jpeg.run.index;
        int r=a733_video_jpeg_poll(&jpeg.run);
        if(r||(jpeg.run.state==state&&jpeg.run.index==index)||++n==JPEG_POLL_BUDGET||!board_live(NULL))return r;
    }
}
void jpeg_task(void *unused)
{
    (void)unused;
    for(;;){
        if(!board_live(NULL)){atomic_store_explicit(&jpeg.result,2,memory_order_release);vTaskDelay(1);continue;}
        if(atomic_load_explicit(&providers.shared.pmic_ready,memory_order_acquire)&&!atomic_load_explicit(&jpeg.result,memory_order_acquire)){
            if(!jpeg_input_ready(&jpeg.supplied)){vTaskDelay(1);continue;}
            int r=jpeg.stage?jpeg_advance():jpeg_setup();
            if(r<0)atomic_store_explicit(&jpeg.result,2,memory_order_release);
            else if(!jpeg.stage&&r==1)jpeg.stage=1;
            else if(jpeg.stage&&r==1&&jpeg.run.state==A733_VIDEO_JPEG_COPIED)atomic_store_explicit(&jpeg.result,1,memory_order_release);
        }
        vTaskDelay(1);
    }
}
