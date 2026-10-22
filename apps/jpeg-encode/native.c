#include "encode.h"
#include "a733_memory_uncached.h"
#include "a733_memory_reservations.h"
#include "a733_storage_digest.h"
#include "a733_mapping_check.h"
#include "a733_mmio.h"
#include "boot.h"
#include <string.h>
struct jpeg_encode_application jpeg_encode;
static uint64_t jpeg_encode_deadline(void){return board.session.deadline_us-10000;}
static int jpeg_encode_now(void *v,uint64_t *out)
{return v==&jpeg_encode&&board_live(NULL)&&a733_counter_clock_now(&providers.shared.counter,out)==1;}
static int jpeg_encode_enter(void *v,uint64_t d)
{return v==&jpeg_encode&&board_live(NULL)?a733_platform_lock_enter(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK,d):-1;}
static int jpeg_encode_leave(void *v)
{if(v!=&jpeg_encode)return -1;a733_platform_lock_leave(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK);return 1;}
static const struct a733_mapping_rule jpeg_encode_device_rule={A733_MAPPING_WRITE,1,{{A733_MAIR_DEVICE_NGNRNE,A733_SH_OUTER}},0,1};
static int jpeg_encode_device(uintptr_t address)
{return a733_mapping_check(aarch64_device_translate,address,address,&jpeg_encode_device_rule);}
static int jpeg_encode_load(uintptr_t address,uint32_t *value)
{if(!jpeg_encode_device(address)||!board_live(NULL))return -1;*value=a733_mmio_read32_dsb_sy_both(address);return board_live(NULL)?1:-1;}
static int jpeg_encode_iommu_read(void *v,uint64_t token,uint32_t off,uint32_t *value,uint64_t d)
{return v==&jpeg_encode&&token==jpeg_encode.session.authority&&d==jpeg_encode_deadline()&&off<0x20000&&!(off&3)?jpeg_encode_load(A733_IOMMU_BASE+off,value):-1;}
static int jpeg_encode_iommu_write(void *v,uint64_t token,uint32_t off,uint32_t value,uint64_t d)
{
    uint32_t ignored;
    if(jpeg_encode_iommu_read(v,token,off,&ignored,d)!=1)return -1;
    if(jpeg_encode_enter(v,d)!=1)return -1;
    a733_mmio_write32_dsb_sy_after(A733_IOMMU_BASE+off,value);
    if(!board_live(NULL))return -1;return jpeg_encode_leave(v);
}
static int jpeg_encode_publish(void *v,uint64_t token,const void *p,size_t n,uint64_t d)
{
    if(v!=&jpeg_encode||token!=jpeg_encode.session.authority||p!=jpeg_encode.table||n!=32768||d!=jpeg_encode_deadline())return -1;
    __asm__ volatile("dsb sy":::"memory");return board_live(NULL)?1:-1;
}
static int jpeg_encode_iommu_admit(void *v,const struct a733_iommu_tables *t,uint64_t d,struct a733_iommu_admission *a)
{
    if(v!=&jpeg_encode||t->words!=jpeg_encode.table||d!=jpeg_encode_deadline()||!board_live(NULL))return -1;
    *a=(struct a733_iommu_admission){.token=jpeg_encode.session.authority,
        .excluded_masters=A733_IOMMU_REAL_MASTERS,.powered_masters=0x8,
        .inherited_resolved=1,.transactions_drained=1,.clocks_held=1,.retained_power_removal=1};
    /* These facts come from the full authenticated initial-isolation record,
     * never inferred from disabled ENABLE, empty fault registers or CPU fences. */
    return a733_clock_token_status(&providers.shared.clocks,&jpeg_encode.iommu_clock)==A733_CLOCK_OK?1:-1;
}
static int authority(void *v,const struct a733_encode_platform_config *c,
    const struct a733_jpeg_encode_job *j,uint64_t d,uint64_t *token)
{
    if(v!=&jpeg_encode||!token)return -1;
    if(*token&&*token!=jpeg_encode.session.authority)return -1;*token=jpeg_encode.session.authority;
    if(!board_live(NULL)||d!=jpeg_encode_deadline()||memcmp(c,&jpeg_encode.config,sizeof(*c))||
       !jpeg_encode_input_ready(&jpeg_encode.supplied)||j->input!=jpeg_encode.input||
       j->input_bytes!=460800||j->input_capacity!=462848||j->output_capacity!=1048576||
       j->input_iova!=0x10000000||j->output_iova!=0x10100000)return -1;
    uint8_t hash[32];storage_digest_bytes(j->input,460800,hash);
    if(memcmp(hash,jpeg_encode.session.fixture,32)||!jpeg_encode_device(0x01c10000))return -1;
    uint32_t gate;
    for(unsigned i=0;i<2;++i)if(jpeg_encode_load(0x02002000+(i?0x5b4:0x58c),&gate)!=1||
       (gate&0x10007)!=jpeg_encode.session.iommu_gates[i])return -1;
    return 1;
}
static int memory(void *v,uint64_t token,const struct a733_jpeg_encode_job *j,uint64_t d)
{
    if(v!=&jpeg_encode||token!=jpeg_encode.session.authority||d!=jpeg_encode_deadline()||
       !jpeg_encode_input_ready(&jpeg_encode.supplied)||j->input!=jpeg_encode.input)return -1;
    if(!jpeg_encode.iommu.initialized){
        struct a733_iommu_ops ops={jpeg_encode_now,jpeg_encode_iommu_admit,jpeg_encode_iommu_read,jpeg_encode_iommu_write,jpeg_encode_publish};
        if(a733_iommu_init_encoder(&jpeg_encode.iommu,&ops,&jpeg_encode,&jpeg_encode.tables)!=1||
           a733_iommu_open(&jpeg_encode.iommu,d)!=1)return -1;
    }
    return a733_iommu_poll(&jpeg_encode.iommu);
}
static int guards(void)
{
    for(unsigned a=0;a<2;++a){
        struct device_memory_extent e;enum device_memory_state state;
        if(!device_memory_describe(jpeg_encode.pool,jpeg_encode.leases[a+1],&e,&state)||state!=DEVICE_MEMORY_DEVICE)return 0;
        size_t size=a?1048576:462848;
        if(e.bytes!=size+8192||e.cpu+4096!=(uintptr_t)(a?jpeg_encode.output:jpeg_encode.input))return 0;
        const volatile uint8_t *p=(const void *)e.cpu;
        for(unsigned i=0;i<4096;++i)if(p[i]!=0xa5||p[4096+size+i]!=0xa5)return 0;
        if(!a)for(size_t i=460800;i<462848;++i)if(p[4096+i]!=0xa5)return 0;
    }
    return 1;
}
static int stable(size_t n)
{
    uint32_t status,count;
    return board_live(NULL)&&guards()&&jpeg_encode_load(0x01c10b1c,&status)==1&&(status&15)==1&&
      jpeg_encode_load(0x01c10b90,&count)==1&&count%8==0&&count/8+2==n&&
      !memcmp(jpeg_encode.output,a733_jpeg_encode_header,623);
}
static int copied(void *v,uint64_t token,size_t n,uint64_t d)
{
    if(v!=&jpeg_encode||token!=jpeg_encode.session.authority||d!=jpeg_encode_deadline()||
       n<626||n>1048576||!board_live(NULL))return -1;
    if(jpeg_encode.jpeg_bytes&&jpeg_encode.jpeg_bytes!=n)return -1;
    jpeg_encode.jpeg_bytes=n;
    if(!jpeg_encode.copy_step){if(a733_iommu_scan(&jpeg_encode.iommu,d)!=1)return -1;jpeg_encode.copy_step=1;}
    int r=a733_iommu_poll(&jpeg_encode.iommu);if(r!=1)return r;
    if(jpeg_encode.iommu.fault.scanned_masters!=8||!stable(n))return -1;
    if(jpeg_encode.copied<n-2){
        size_t chunk=n-2-jpeg_encode.copied;if(chunk>32768)chunk=32768;
        __asm__ volatile("dsb sy":::"memory");
        memcpy(jpeg_encode.payload+jpeg_encode.copied,jpeg_encode.output+jpeg_encode.copied,chunk);
        __asm__ volatile("dsb sy":::"memory");
        if(!stable(n))return -1;jpeg_encode.copied+=chunk;
        if(a733_iommu_scan(&jpeg_encode.iommu,d)!=1)return -1;return 0;
    }
    jpeg_encode.payload[n-2]=0xff;jpeg_encode.payload[n-1]=0xd9;
    storage_digest_bytes(jpeg_encode.payload,n,jpeg_encode.digest);
    return stable(n)?1:-1;
}
int jpeg_encode_upload_allocate(void)
{
    if(jpeg_encode.wb_pool||jpeg_encode.supplied.bytes||jpeg_encode.payload)return 0;
    jpeg_encode.wb_pool=a733_memory_reservations_init();if(!jpeg_encode.wb_pool)return 0;
    for(unsigned i=0;i<2;++i){
        struct device_memory_extent e;size_t size=i?1048576:462848;
        if(!device_memory_acquire_aligned(jpeg_encode.wb_pool,jpeg_encode.session.owner,size,4096,
           &jpeg_encode.wb_leases[i],&e)||e.bytes!=size||e.cpu%4096)return 0;
        if(i)jpeg_encode.payload=(void *)e.cpu;
        else if(!jpeg_encode_input_bind(&jpeg_encode.supplied,(void *)e.cpu,e.bytes))return 0;
    }
    return 1;
}
__attribute__((noinline)) int jpeg_encode_allocate(void)
{
    jpeg_encode.pool=a733_memory_uncached_init();if(!jpeg_encode.pool)return 0;
    for(unsigned i=0;i<3;++i){
        size_t size=i==0?32768:i==1?471040:1056768;
        struct device_memory_extent e;
        if(!device_memory_acquire_aligned(jpeg_encode.pool,jpeg_encode.session.owner,size,i?4096:16384,
           &jpeg_encode.leases[i],&e)||e.bytes!=size||e.cpu%(i?4096:16384))return 0;
        if(!i)jpeg_encode.table=(void *)e.cpu;
        else{memset((void *)e.cpu,0xa5,e.bytes);if(i==1)jpeg_encode.input=(void *)(e.cpu+4096);else jpeg_encode.output=(void *)(e.cpu+4096);}
    }
    memcpy(jpeg_encode.input,jpeg_encode.supplied.bytes,460800);
    memcpy(jpeg_encode.output,a733_jpeg_encode_header,623);
    struct a733_iommu_span spans[]={{0x10000000,(uintptr_t)jpeg_encode.input,462848,A733_IOMMU_READ},
      {0x10100000,(uintptr_t)jpeg_encode.output,1048576,A733_IOMMU_READ|A733_IOMMU_WRITE}};
    if(a733_iommu_build(&jpeg_encode.tables,jpeg_encode.table,(uintptr_t)jpeg_encode.table,spans,2)!=1)return 0;
    __asm__ volatile("dsb sy":::"memory");
    for(unsigned i=0;i<3;++i){struct device_memory_allocation *a=device_memory_lookup(jpeg_encode.pool,jpeg_encode.leases[i]);
        if(!a||a->state!=DEVICE_MEMORY_CPU)return 0;a->state=DEVICE_MEMORY_DEVICE;}
    return guards();
}
static int setup(void)
{
    struct a733_encode_platform_config *c=&jpeg_encode.config;
    if(!c->owner)*c=(struct a733_encode_platform_config){.clocks=&providers.shared.clocks,.power=&providers.shared.gpu,
      .power_client=(unsigned)jpeg_encode.session.pck_client,.owner=jpeg_encode.session.owner,
      .handoff=jpeg_encode.session.board.generation,.module=jpeg_encode.session.module|0x80000000,
      .module_hz=jpeg_encode.session.module_hz};
    memcpy(c->fabric,jpeg_encode.session.board.fabric,sizeof(c->fabric));
    memcpy(c->fabric_hz,jpeg_encode.session.board.fabric_hz,sizeof(c->fabric_hz));
    enum a733_rail_result rr;
    if(!jpeg_encode.rail.id)rr=a733_rail_acquire(&providers.shared.rails,c->owner,
      A733_RAIL_BIT(jpeg_encode.session.supply_rail),0,jpeg_encode_deadline(),&jpeg_encode.rail);
    else rr=a733_rail_poll(&providers.shared.rails,&jpeg_encode.rail);
    if(rr==A733_RAIL_BUSY||rr==A733_RAIL_PENDING)return 0;if(rr!=A733_RAIL_READY)return -1;
    enum a733_pmic_status pr;
    if(!jpeg_encode.request.id){pr=a733_pmic_submit(&providers.shared.pmic,c->owner,&jpeg_encode.rail,
      (enum a733_rail)jpeg_encode.session.supply_rail,jpeg_encode_deadline(),&jpeg_encode.request);
      return pr==A733_PMIC_PENDING||pr==A733_PMIC_BUSY?0:-1;}
    pr=a733_pmic_result(&providers.shared.pmic,&jpeg_encode.request,&jpeg_encode.supply);
    if(pr==A733_PMIC_PENDING||pr==A733_PMIC_BUSY)return 0;
    if(pr!=A733_PMIC_COMPLETE||jpeg_encode.supply.rail!=jpeg_encode.session.supply_rail||
      jpeg_encode.supply.page_before||jpeg_encode.supply.page_after||jpeg_encode.supply.chip!=3||jpeg_encode.supply.revision||
      !jpeg_encode.supply.enabled||!jpeg_encode.supply.decoded||jpeg_encode.supply.programmed_uv!=jpeg_encode.session.supply_uv)return -1;
    a733_clock_mask_t scopes;if(!a733_encode_platform_scopes(c,&scopes))return -1;
    if(!c->clock.id){enum a733_clock_result r=a733_clock_acquire(c->clocks,c->owner,scopes,A733_CLOCK_VE_ENC,jpeg_encode_deadline(),&c->clock);
      if(r==A733_CLOCK_BUSY)return 0;if(r!=A733_CLOCK_OK)return -1;}
    if(!jpeg_encode.iommu_clock.id){enum a733_clock_result r=a733_clock_acquire(c->clocks,c->owner,A733_CLOCK_FABRIC|A733_CLOCK_REF|A733_CLOCK_OSC,
      A733_CLOCK_IOMMU_SYS|A733_CLOCK_VI|A733_CLOCK_DE_SYS|A733_CLOCK_VE_DEC,jpeg_encode_deadline(),&jpeg_encode.iommu_clock);
      if(r==A733_CLOCK_BUSY)return 0;if(r!=A733_CLOCK_OK)return -1;}
    if(!jpeg_encode.power_token){enum a733_gpu_resource_result r=a733_gpu_resources_acquire(c->power,c->power_client,
      A733_GPU_RESOURCE_OSC|A733_GPU_RESOURCE_PCK,jpeg_encode_deadline(),&jpeg_encode.power_token,&jpeg_encode.power_ready);
      if(r==A733_GPU_RESOURCE_BUSY)return 0;if(r!=A733_GPU_RESOURCE_OK)return -1;}
    c->pck=(struct a733_gpu_resource_lease){jpeg_encode.power_token,A733_GPU_RESOURCE_OSC|A733_GPU_RESOURCE_PCK,A733_GPU_RESOURCE_STABLE};
    struct a733_encode_platform_ops ops={jpeg_encode_now,authority,memory,copied,jpeg_encode_enter,jpeg_encode_leave,a733_encode_native_read,a733_encode_native_write};
    if(!jpeg_encode_allocate()||!a733_encode_platform_init(&jpeg_encode.platform,c,&ops,&jpeg_encode))return -1;
    struct a733_jpeg_encode_job j={jpeg_encode.input,460800,0x10000000,0x10100000,462848,1048576};
    return a733_jpeg_encode_begin(&jpeg_encode.run,a733_encode_platform_jpeg_ops(),&jpeg_encode.platform,&j,jpeg_encode_deadline())?1:-1;
}
/* The controller performs at most one register operation per poll. Repeat
 * polls while each one advances its state or index, so programming sequences
 * finish in one wake. Waiting for hardware, a copy step or a busy resource
 * makes no progress and sleeps a tick; the budget bounds one wake's work. */
#define POLL_BUDGET 1024u
static int advance(void)
{
    for(unsigned n=0;;){
        enum a733_jpeg_encode_state state=jpeg_encode.run.state;unsigned index=jpeg_encode.run.index;
        int r=a733_jpeg_encode_poll(&jpeg_encode.run);
        if(r||(jpeg_encode.run.state==state&&jpeg_encode.run.index==index)||++n==POLL_BUDGET||!board_live(NULL))return r;
    }
}
void jpeg_encode_task(void *unused)
{
    (void)unused;for(;;){
      if(!board_live(NULL)){atomic_store_explicit(&jpeg_encode.result,2,memory_order_release);vTaskDelay(1);continue;}
      if(atomic_load_explicit(&providers.shared.pmic_ready,memory_order_acquire)&&!atomic_load_explicit(&jpeg_encode.result,memory_order_acquire)){
        if(!jpeg_encode_input_ready(&jpeg_encode.supplied)){vTaskDelay(1);continue;}
        int r=jpeg_encode.stage?advance():setup();
        if(r<0)atomic_store_explicit(&jpeg_encode.result,2,memory_order_release);
        else if(!jpeg_encode.stage&&r==1)jpeg_encode.stage=1;
        else if(jpeg_encode.stage&&r==1&&jpeg_encode.run.state==A733_JPEG_ENCODE_COPIED)
          atomic_store_explicit(&jpeg_encode.result,1,memory_order_release);
      }vTaskDelay(1);
    }
}
