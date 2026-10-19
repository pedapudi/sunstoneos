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
{return v==&h264&&board_live(NULL)&&(h264.session.version!=H264_STREAM_SESSION_VERSION||h264_stream_authorized(&h264_stream,&h264.run))?a733_platform_lock_enter(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK,d):-1;}
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
       (h264.session.version==H264_STREAM_SESSION_VERSION?!h264_stream_authorized(&h264_stream,&h264.run):
        (!a733_h264_input_ready(&h264.supplied)||j->input!=h264.supplied.stream||
         j->input_bytes!=h264.supplied.stream_bytes)))return -1;
    for(unsigned i=0;i<A733_H264_BUFFERS;++i)if(j->buffers[i].iova!=h264.job.buffers[i].iova||
        j->buffers[i].bytes!=h264.job.buffers[i].bytes)return -1;
    uint8_t hash[32];
    if(h264.session.version==H264_STREAM_SESSION_VERSION){
        if(j->profile!=H264_STREAM_PROFILE||j->input!=h264_stream.input||j->input_bytes!=H264_STREAM_INPUT_BYTES)return -1;
        storage_digest_bytes(j->input,h264_stream.input_bytes,hash);
        if(memcmp(hash,h264_stream_descriptor(&h264_stream,h264_stream.active)+16,32))return -1;
    }else{storage_digest_bytes(j->input,j->input_bytes,hash);if(memcmp(hash,h264.session.fixture,32))return -1;}
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
static const uint32_t slot_capacity[]={4096,4096,4096,0x21000,4096,0xc000};
static const uint32_t capacity[]={8192,8192,8192,0x21000,8192,0xc000};
static const unsigned slots[]={2,2,2,1,2,1};
static const uint32_t used[]={4096,2048,1024,A733_H264_PICTURE_INFO_BYTES,2048,0xc000};
static const uint32_t prefix[]={4096,4096,4096,4096,4096,16384};
#ifdef SUNSTONE_H264_FULLHD
static const uint32_t vga_slot_capacity[]={4194304,2088960,1044480,0x35000,262144,0xc000};
static const uint32_t vga_capacity[]={4194304,4177920,2088960,0x35000,524288,0xc000};
static const uint32_t vga_used[]={4194304,2088960,1044480,0x34000,262144,0xc000};
#else
static const uint32_t vga_slot_capacity[]={524288,307200,155648,0x22000,40960,0xc000};
static const uint32_t vga_capacity[]={524288,614400,311296,0x22000,81920,0xc000};
static const uint32_t vga_used[]={524288,307200,153600,0x21000,38912,0xc000};
#endif
static const uint32_t vga_slots[]={1,2,2,1,2,1};
static int vga(void){return h264.session.version==H264_STREAM_SESSION_VERSION;}
static int h264_guards(void)
{
    const uint32_t *cap=vga()?vga_capacity:capacity,*step=vga()?vga_slot_capacity:slot_capacity;
    const uint32_t *limit=vga()?vga_used:used;const unsigned *count=vga()?vga_slots:slots;
    for(unsigned a=0;a<A733_H264_BUFFERS;++a){
        struct device_memory_extent e;enum device_memory_state state;
        if(!device_memory_describe(h264.pool,h264.leases[a+1],&e,&state)||state!=((vga()&&a==A733_H264_INPUT&&h264.run.state==A733_H264_COPIED)?DEVICE_MEMORY_CPU:DEVICE_MEMORY_DEVICE))return 0;
        const volatile uint8_t *p=(const void *)e.cpu;
        if(e.bytes!=prefix[a]+cap[a]+4096||e.cpu+prefix[a]!=(uintptr_t)h264.buffers[a])return 0;
        for(unsigned i=0;i<prefix[a];++i)if(p[i]!=0xa5)return 0;
        for(unsigned slot=0;slot<count[a];++slot)
            for(unsigned i=prefix[a]+slot*step[a]+limit[a];
                i<prefix[a]+(slot+1)*step[a];++i)if(p[i]!=0xa5)return 0;
        for(unsigned i=prefix[a]+cap[a];i<e.bytes;++i)if(p[i]!=0xa5)return 0;
    }
    return 1;
}
#ifdef SUNSTONE_H264_FULLHD
static int h264_fullhd_copied(struct h264_stream_output *out,uint64_t deadline)
{
    if(!out||!h264_stream_storage_valid()||atomic_load_explicit(&h264_stream.failed,memory_order_acquire)||
       atomic_load_explicit(&out->state,memory_order_acquire)!=1)return -1;
    if(!h264.copy_step){
        if(a733_iommu_scan(&h264.iommu,deadline)!=1)return -1;
        h264.copy_step=1;return 0;
    }
    if(h264.copy_step!=3){
        int scanned=a733_iommu_poll(&h264.iommu);if(scanned!=1)return scanned;
        if(!h264.iommu.fault.complete||h264.iommu.fault.scanned_masters!=0xc0)return -1;
    }
    uint32_t status;
    if(!h264_guards()||h264_load(0x01c0e228,&status)!=1||(status&(A733_H264_BUSY_MASK|0xe))||!board_live(NULL))return -1;
    if(h264.copy_step==2){
        storage_digest_end(&h264.copy_hash,out->digest);h264.copy_step=0;return 1;
    }
    if(h264.copy_step==1){
        h264.copy_plane=h264.copy_row=0;storage_digest_init(&h264.copy_hash);h264.copy_step=3;
    }
    /* At most32rows (61440bytes), including oracle/hash work, per callback.
     * Cr uses coded544-row Cb storage, not the visible540-row copy extent. */
    unsigned plane=h264.copy_plane,row=h264.copy_row;
    unsigned width=plane?960:1920,height=plane?540:1080;
    unsigned count=height-row;if(count>32)count=32;
    size_t offset=plane?2073600+(plane-1)*518400+(size_t)row*width:(size_t)row*width;
    unsigned slot=h264.run.picture_index&1;
    const volatile uint8_t *source=plane?h264.chroma+slot*1044480+(plane-1)*522240:h264.luma+slot*2088960;
    uint8_t *destination=out->payload+offset;
    __asm__ volatile("dsb sy":::"memory");
    for(unsigned y=0;y<count;++y)for(unsigned x=0;x<width;++x)destination[(size_t)y*width+x]=source[(size_t)(row+y)*width+x];
    __asm__ volatile("dsb sy":::"memory");
    size_t bytes=(size_t)count*width;
    if((h264_stream.oracle_bytes&&memcmp(destination,h264_stream.oracle+offset,bytes))||
       !storage_digest_add(&h264.copy_hash,destination,bytes)||!h264_stream_storage_valid()||
       atomic_load_explicit(&h264_stream.failed,memory_order_acquire)||!board_live(NULL))return -1;
    h264.copy_row+=count;
    if(h264.copy_row==height){h264.copy_row=0;++h264.copy_plane;}
    if(h264.copy_plane==3){
        if(a733_iommu_scan(&h264.iommu,deadline)!=1)return -1;
        h264.copy_step=2;
    }
    return 0;
}
#endif
static int h264_copied(void *v,uint64_t token,uint64_t d)
{
    if(v!=&h264||token!=h264.session.authority||d!=h264_deadline()||!board_live(NULL))return -1;
    unsigned picture=h264.run.picture_index;
    if(vga()?!h264_stream_authorized(&h264_stream,&h264.run):
       (picture>=h264.supplied.sequence.count||picture!=atomic_load_explicit(&h264.completed,memory_order_acquire)))return -1;
    struct h264_stream_output *out=vga()?&h264_stream.output[h264_stream.copy_slot]:NULL;
    uint8_t *payload=vga()?out->payload:h264.payload[picture];
    uint8_t *digest=vga()?out->digest:h264.digest[picture];
#ifdef SUNSTONE_H264_FULLHD
    if(vga())return h264_fullhd_copied(out,d);
#endif
    unsigned width=vga()?640:48,height=vga()?480:32,ystride=vga()?640:64,cstride=vga()?320:32;
    unsigned yslot=vga()?(picture&1)*307200:picture*4096,cslot=vga()?(picture&1)*155648:picture*4096;
    unsigned ybytes=width*height,cbytes=ybytes/4,cplane=cstride*height/2;
    if(!h264.copy_step){if(a733_iommu_scan(&h264.iommu,d)!=1)return -1;h264.copy_step=1;}
    int r=a733_iommu_poll(&h264.iommu);if(r!=1)return r;
    if(h264.iommu.fault.scanned_masters!=0xc0||!h264_guards())return -1;
    if(h264.copy_step==2){
        uint32_t final_status;
        if(h264_load(0x01c0e228,&final_status)!=1||
           (final_status&(A733_H264_BUSY_MASK|0xe))||!h264_guards())return -1;
        storage_digest_bytes(payload,ybytes+2*cbytes,digest);
        h264.copy_step=0;
        return 1;
    }
    uint32_t status;
    if(h264_load(0x01c0e228,&status)!=1||(status&(A733_H264_BUSY_MASK|0xe)))return -1;
    __asm__ volatile("dsb sy":::"memory");
    for(unsigned row=0;row<height;++row)for(unsigned col=0;col<width;++col)payload[row*width+col]=((volatile uint8_t *)(h264.luma+yslot))[row*ystride+col];
    for(unsigned plane=0;plane<2;++plane)for(unsigned row=0;row<height/2;++row)for(unsigned col=0;col<width/2;++col)
        payload[ybytes+plane*cbytes+row*(width/2)+col]=((volatile uint8_t *)(h264.chroma+cslot))[plane*cplane+row*cstride+col];
    __asm__ volatile("dsb sy":::"memory");
    if(!h264_guards()||(vga()?(h264_stream.oracle_bytes&&memcmp(payload,h264_stream.oracle,A733_H264_VGA_OUTPUT_BYTES)):
       (h264.supplied.oracle_bytes&&memcmp(payload,h264.supplied.oracle+picture*2304,2304)))||!board_live(NULL))return -1;
    if(a733_iommu_scan(&h264.iommu,d)!=1)return -1;
    h264.copy_step=2;return 0;
}
static int h264_allocate(void)
{
    h264.pool=a733_memory_uncached_init();if(!h264.pool)return 0;
    const uint32_t *cap=vga()?vga_capacity:capacity,*step=vga()?vga_slot_capacity:slot_capacity;
    const uint32_t *limit=vga()?vga_used:used;const unsigned *count=vga()?vga_slots:slots;
    for(unsigned i=0;i<1+A733_H264_BUFFERS;++i){
        struct device_memory_extent e;
        uint64_t bytes=i?prefix[i-1]+cap[i-1]+4096:32768;
        uint64_t alignment=!i||i==1+A733_H264_NEIGHBOR?16384:4096;
        if(!device_memory_acquire_aligned(h264.pool,h264.session.owner,bytes,alignment,&h264.leases[i],&e))return 0;
        if(i==0)h264.table=(void *)e.cpu;
        else{memset((void *)e.cpu,0xa5,e.bytes);h264.buffers[i-1]=(void *)(e.cpu+prefix[i-1]);
            for(unsigned slot=0;slot<count[i-1];++slot)
                memset(h264.buffers[i-1]+slot*step[i-1],0,limit[i-1]);}
    }
    h264.input=h264.buffers[A733_H264_INPUT];h264.luma=h264.buffers[A733_H264_Y];h264.chroma=h264.buffers[A733_H264_C];
    const struct a733_h264_sequence *sequence=&h264.supplied.sequence;
    for(unsigned slot=0;!vga()&&slot<sequence->count;++slot){
        const struct a733_h264_picture *p=&sequence->pictures[slot];
        memcpy(h264.input+slot*4096,h264.supplied.stream+p->slice_offset,p->slice_bytes);
    }
    if(vga())memcpy(h264.input,h264_stream.input+h264_stream.picture.slice_offset,h264_stream.picture.slice_bytes);
    static const uint32_t addresses[2][A733_H264_BUFFERS]={
        {0x10001000,0x10010000,0x10014000,0x10020000,0x10068000,0x10070000},
        {0x10000000,0x10100000,0x10200000,0x10300000,0x10400000,0x10500000}};
#ifdef SUNSTONE_H264_FULLHD
    static const uint32_t hd_addresses[]={0x10000000,0x10401000,0x10801000,0x10a00000,0x10a36000,0x10abc000};
    const uint32_t *iova=vga()?hd_addresses:addresses[0];
#else
    const uint32_t *iova=addresses[vga()];
#endif
    struct a733_iommu_span spans[A733_H264_BUFFERS];
    h264.job.input=vga()?h264_stream.input:h264.supplied.stream;
    h264.job.input_bytes=vga()?H264_STREAM_INPUT_BYTES:h264.supplied.stream_bytes;
    h264.job.profile=vga()?H264_STREAM_PROFILE:A733_H264_LEGACY;
    for(unsigned i=0;i<A733_H264_BUFFERS;++i){
        h264.job.buffers[i]=(struct a733_h264_span){iova[i],cap[i]};
        spans[i]=(struct a733_iommu_span){iova[i],(uintptr_t)h264.buffers[i],cap[i],
            i==A733_H264_INPUT?A733_IOMMU_READ:A733_IOMMU_READ|A733_IOMMU_WRITE};
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
    return (vga()?a733_h264_stream_begin(&h264.run,a733_video_platform_h264_ops(),&h264.platform,&h264.job,h264_stream.input_bytes,h264_deadline()):
        a733_h264_begin(&h264.run,a733_video_platform_h264_ops(),&h264.platform,&h264.job,h264_deadline()))?1:-1;
}
static void h264_vga_failure(void)
{
    h264_stream_fail(&h264_stream);h264_stream_storage_retain();atomic_store_explicit(&h264.result,2,memory_order_release);
    if(h264.pool)for(unsigned i=0;i<1+A733_H264_BUFFERS;++i)
        if(h264.leases[i].generation)device_memory_retain(h264.pool,h264.leases[i]);
}
static void h264_vga_step(void)
{
    if(atomic_load_explicit(&h264_stream.failed,memory_order_acquire)){h264_vga_failure();return;}
    if(atomic_load_explicit(&h264.result,memory_order_acquire))return;
    if(atomic_load_explicit(&h264_stream.au_state,memory_order_acquire)!=2){
        int taken=h264_stream_take(&h264_stream);if(taken<0){h264_vga_failure();return;}if(!taken)return;
        if(h264.stage)h264.refill_step=1;
    }
    if(h264.refill_step){
        struct device_memory_allocation *input=device_memory_lookup(h264.pool,h264.leases[1]);
        if(h264.run.state!=A733_H264_COPIED||!input||input->state!=DEVICE_MEMORY_CPU||!board_live(NULL)){h264_vga_failure();return;}
        if(h264.refill_step==1){
            if(!h264_guards()||a733_iommu_scan(&h264.iommu,h264_deadline())!=1){h264_vga_failure();return;}
            h264.refill_step=2;return;
        }
        int scanned=a733_iommu_poll(&h264.iommu);
        if(!scanned)return;
        if(scanned!=1||!h264.iommu.fault.complete||h264.iommu.fault.scanned_masters!=0xc0||
           !h264_guards()||!board_live(NULL)){h264_vga_failure();return;}
        memset(h264.input,0,H264_STREAM_INPUT_BYTES);
        memcpy(h264.input,h264_stream.input+h264_stream.picture.slice_offset,h264_stream.picture.slice_bytes);
        __asm__ volatile("dsb sy":::"memory");input->state=DEVICE_MEMORY_DEVICE;
        if(a733_h264_stream_next(&h264.run,h264_stream.input_bytes,h264_stream.picture.frame_num)!=1){h264_vga_failure();return;}
        h264.refill_step=0;
    }
    int result=h264.stage?a733_h264_poll(&h264.run):h264_setup();
    if(result<0){h264_vga_failure();return;}
    if(!h264.stage){if(result==1)h264.stage=1;return;}
    if(result==1&&h264.run.state==A733_H264_COPIED){
        struct device_memory_allocation *input=device_memory_lookup(h264.pool,h264.leases[1]);
        if(!board_live(NULL)||!input||input->state!=DEVICE_MEMORY_DEVICE){h264_vga_failure();return;}
        /* Source-backed successful job completion returns this input slot to
         * its controller. Reference/output/MV mappings remain device-owned. */
        input->state=DEVICE_MEMORY_CPU;
        unsigned final=h264_stream.active+1==h264_stream.count;
        if(h264_stream_complete(&h264_stream)!=1){h264_vga_failure();return;}
        if(final)atomic_store_explicit(&h264.result,1,memory_order_release);
    }
}
void h264_task(void *unused)
{
    (void)unused;
    for(;;){
        if(!board_live(NULL)){if(vga())h264_vga_failure();else atomic_store_explicit(&h264.result,2,memory_order_release);vTaskDelay(1);continue;}
        if(vga()){if(atomic_load_explicit(&providers.shared.pmic_ready,memory_order_acquire))h264_vga_step();vTaskDelay(1);continue;}
        if(a733_h264_input_ready(&h264.supplied)&&
           atomic_load_explicit(&providers.shared.pmic_ready,memory_order_acquire)&&!atomic_load_explicit(&h264.result,memory_order_acquire)){
            int r=h264.stage?a733_h264_poll(&h264.run):h264_setup();
            if(r>=0&&h264.stage&&h264.run.picture_index>
               atomic_load_explicit(&h264.completed,memory_order_acquire)){
                if(!board_live(NULL))r=-1;
                else atomic_store_explicit(&h264.completed,h264.run.picture_index,memory_order_release);
            }
            if(r<0)atomic_store_explicit(&h264.result,2,memory_order_release);
            else if(!h264.stage&&r==1)h264.stage=1;
            else if(h264.stage&&r==1&&h264.run.state==A733_H264_COPIED)atomic_store_explicit(&h264.result,1,memory_order_release);
        }
        vTaskDelay(1);
    }
}
