#include "hevc.h"
#include "a733_storage_digest.h"
#include "aarch64_device_memory.h"
#include "boot.h"
#include <string.h>
struct hevc_application hevc;
#define HEVC_STATUS UINT32_C(0x01c0e538)
/* Output bytes copied per retained-copy call. It bounds the uncached reads
 * in one task wake; the task sleeps a tick between calls. */
#define HEVC_COPY_BUDGET 65536u
_Static_assert(A733_HEVC_PICTURE_BYTES%8==0&&HEVC_COPY_BUDGET%8==0,"word copy");
static uint64_t hevc_deadline(void){return board.session.deadline_us-10000;}
static int hevc_now(void *v,uint64_t *out)
{return v==&hevc&&board_live(NULL)&&a733_counter_clock_now(&providers.shared.counter,out)==1;}
static int hevc_enter(void *v,uint64_t d)
{return v==&hevc&&board_live(NULL)?a733_platform_lock_enter(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK,d):-1;}
static int hevc_leave(void *v)
{if(v!=&hevc)return -1;a733_platform_lock_leave(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK);return 1;}
static int hevc_device(uintptr_t address)
{
    struct aarch64_translation t;
    return aarch64_device_translate(address,1,&t)&&t.physical==address&&t.attributes==0&&t.shareability==2;
}
static int hevc_load(uintptr_t address,uint32_t *value)
{if(!hevc_device(address)||!board_live(NULL))return -1;__asm__ volatile("dsb sy":::"memory");*value=*(volatile uint32_t *)address;__asm__ volatile("dsb sy":::"memory");return board_live(NULL)?1:-1;}
static int hevc_iommu_read(void *v,uint64_t token,uint32_t off,uint32_t *value,uint64_t d)
{return v==&hevc&&token==hevc.session.authority&&d==hevc_deadline()&&off<0x20000&&!(off&3)?hevc_load(A733_IOMMU_BASE+off,value):-1;}
static int hevc_iommu_write(void *v,uint64_t token,uint32_t off,uint32_t value,uint64_t d)
{
    uint32_t ignored;
    if(hevc_iommu_read(v,token,off,&ignored,d)!=1)return -1;
    if(hevc_enter(v,d)!=1)return -1;
    *(volatile uint32_t *)(uintptr_t)(A733_IOMMU_BASE+off)=value;
    __asm__ volatile("dsb sy":::"memory");
    if(!board_live(NULL))return -1;return hevc_leave(v);
}
static int hevc_publish(void *v,uint64_t token,const void *p,size_t n,uint64_t d)
{
    if(v!=&hevc||token!=hevc.session.authority||p!=hevc.storage.table||n!=A733_IOMMU_TABLE_BYTES||d!=hevc_deadline())return -1;
    __asm__ volatile("dsb sy":::"memory");return board_live(NULL)?1:-1;
}
static int hevc_iommu_admit(void *v,const struct a733_iommu_tables *t,uint64_t d,struct a733_iommu_admission *a)
{
    if(v!=&hevc||t->words!=hevc.storage.table||d!=hevc_deadline()||!board_live(NULL))return -1;
    *a=(struct a733_iommu_admission){.token=hevc.session.authority,
        .excluded_masters=A733_IOMMU_REAL_MASTERS,.powered_masters=0xc0,
        .inherited_resolved=1,.transactions_drained=1,.clocks_held=1,.retained_power_removal=1};
    /* These facts come from the full authenticated initial-isolation record,
     * never inferred from disabled ENABLE, empty fault registers or CPU fences. */
    return a733_clock_token_status(&providers.shared.clocks,&hevc.iommu_clock)==A733_CLOCK_OK?1:-1;
}
/* The job must be the published storage job: the committed upload, its
 * parsed plan and the fixed spans. */
static int hevc_authority(void *v,const struct a733_video_platform_config *c,
    const struct a733_hevc_job *j,uint64_t d,uint64_t *token)
{
    if(v!=&hevc||!token)return -1;
    if(*token&&*token!=hevc.session.authority)return -1;*token=hevc.session.authority;
    if(!board_live(NULL)||d!=hevc_deadline()||memcmp(c,&hevc.config,sizeof(*c))||
       !hevc_input_ready(&hevc.supplied)||!hevc.storage.published||
       j->input!=hevc.storage.upload||j->input_bytes!=hevc.supplied.total||
       memcmp(&j->plan,&hevc.supplied.plan,sizeof(j->plan))||
       memcmp(j->buffers,hevc.storage.job.buffers,sizeof(j->buffers)))return -1;
    uint8_t hash[32];storage_digest_bytes(j->input,j->input_bytes,hash);
    if(memcmp(hash,hevc.session.input_digest,32))return -1;
    for(uintptr_t p=0x17000;p<0x22000;p+=4096)if(!hevc_device(p))return -1;
    for(uintptr_t p=0x01c0e000;p<0x01c0f000;p+=4096)if(!hevc_device(p))return -1;
    uint32_t gate;
    for(unsigned i=0;i<2;++i)if(hevc_load(0x02002000+(i?0x5b4:0x58c),&gate)!=1||
       (gate&0x10007)!=hevc.session.iommu_gates[i])return -1;
    return 1;
}
static int hevc_memory(void *v,uint64_t token,const struct a733_hevc_job *j,uint64_t d)
{
    (void)j;
    if(v!=&hevc||token!=hevc.session.authority||d!=hevc_deadline())return -1;
    if(!hevc.iommu.initialized){
        struct a733_iommu_ops ops={hevc_now,hevc_iommu_admit,hevc_iommu_read,hevc_iommu_write,hevc_publish};
        if(a733_iommu_init_decoder(&hevc.iommu,&ops,&hevc,&hevc.storage.tables)!=1||a733_iommu_open(&hevc.iommu,d)!=1)return -1;
    }
    return a733_iommu_poll(&hevc.iommu);
}
/* Output and copies are page aligned and the budget is a multiple of eight.
 * Builds use strict alignment, so uncached reads use aligned 64-bit words. */
typedef uint64_t hevc_word __attribute__((may_alias));
static void hevc_copy_words(uint8_t *d,const uint8_t *s,size_t n)
{for(size_t i=0;i<n;i+=8)*(hevc_word *)(d+i)=*(const volatile hevc_word *)(s+i);}
static int hevc_quiet(void)
{
    uint32_t status;
    return hevc_storage_guards(&hevc.storage)&&hevc_load(HEVC_STATUS,&status)==1&&
        !(status&(A733_HEVC_BUSY_MASK|14));
}
/* Fault scan, guards and idle status before the first chunk; guards and a
 * second fault scan after the last. The digest covers the owned copy. */
static int hevc_copied(void *v,uint64_t token,unsigned picture,uint64_t d)
{
    if(v!=&hevc||token!=hevc.session.authority||d!=hevc_deadline()||!board_live(NULL)||
       picture>1||picture!=hevc.run.picture||
       picture!=atomic_load_explicit(&hevc.completed,memory_order_acquire))return -1;
    if(!hevc.copy_step){if(a733_iommu_scan(&hevc.iommu,d)!=1)return -1;hevc.copy_step=1;}
    int r=a733_iommu_poll(&hevc.iommu);if(r!=1)return r;
    if(!hevc.iommu.fault.complete||hevc.iommu.fault.scanned_masters!=0xc0)return -1;
    if(hevc.copy_step==2){
        if(!hevc_quiet())return -1;
        storage_digest_end(&hevc.copy_hash,hevc.digest[picture]);hevc.copy_step=0;return 1;
    }
    if(hevc.copy_step==1){
        if(!hevc_quiet())return -1;
        hevc.copy_offset=0;storage_digest_init(&hevc.copy_hash);hevc.copy_step=3;
    }
    size_t n=A733_HEVC_PICTURE_BYTES-hevc.copy_offset;if(n>HEVC_COPY_BUDGET)n=HEVC_COPY_BUDGET;
    uint8_t *destination=hevc.storage.copies[picture]+hevc.copy_offset;
    __asm__ volatile("dsb sy":::"memory");
    hevc_copy_words(destination,hevc.storage.buffers[HEVC_OUTPUT0+picture]+hevc.copy_offset,n);
    __asm__ volatile("dsb sy":::"memory");
    if(!storage_digest_add(&hevc.copy_hash,destination,n)||!board_live(NULL))return -1;
    hevc.copy_offset+=n;
    if(hevc.copy_offset<A733_HEVC_PICTURE_BYTES)return 0;
    if(!hevc_storage_guards(&hevc.storage)||a733_iommu_scan(&hevc.iommu,d)!=1)return -1;
    hevc.copy_step=2;return 0;
}
static int hevc_setup(void)
{
    struct a733_video_platform_config *c=&hevc.config;
    if(!c->owner)*c=(struct a733_video_platform_config){.clocks=&providers.shared.clocks,.power=&providers.shared.gpu,
        .power_client=(unsigned)hevc.session.pck_client,.owner=hevc.session.owner,.handoff=hevc.session.board.generation,
        .module=hevc.session.module|0x80000000,.module_hz=hevc.session.module_hz,.start=A733_VIDEO_COLD_NS_ROM};
    memcpy(c->fabric,hevc.session.board.fabric,sizeof(c->fabric));memcpy(c->fabric_hz,hevc.session.board.fabric_hz,sizeof(c->fabric_hz));
    enum a733_rail_result rr;
    if(!hevc.rail.id)rr=a733_rail_acquire(&providers.shared.rails,c->owner,
        A733_RAIL_BIT(hevc.session.supply_rail),0,hevc_deadline(),&hevc.rail);
    else rr=a733_rail_poll(&providers.shared.rails,&hevc.rail);
    if(rr==A733_RAIL_BUSY||rr==A733_RAIL_PENDING)return 0;
    if(rr!=A733_RAIL_READY)return -1;
    enum a733_pmic_status pr;
    if(!hevc.request.id){
        pr=a733_pmic_submit(&providers.shared.pmic,c->owner,&hevc.rail,
            (enum a733_rail)hevc.session.supply_rail,hevc_deadline(),&hevc.request);
        return pr==A733_PMIC_PENDING||pr==A733_PMIC_BUSY?0:-1;
    }
    pr=a733_pmic_result(&providers.shared.pmic,&hevc.request,&hevc.supply);
    if(pr==A733_PMIC_PENDING||pr==A733_PMIC_BUSY)return 0;
    if(pr!=A733_PMIC_COMPLETE||hevc.supply.rail!=hevc.session.supply_rail||
       hevc.supply.page_before||hevc.supply.page_after||hevc.supply.chip!=3||hevc.supply.revision||
       !hevc.supply.enabled||!hevc.supply.decoded||hevc.supply.programmed_uv!=hevc.session.supply_uv)return -1;
    a733_clock_mask_t scopes;if(!a733_video_platform_scopes(c,&scopes))return -1;
    if(!c->clock.id){enum a733_clock_result r=a733_clock_acquire(c->clocks,c->owner,scopes,A733_CLOCK_VE_DEC,hevc_deadline(),&c->clock);if(r==A733_CLOCK_BUSY)return 0;if(r!=A733_CLOCK_OK)return -1;}
    if(!hevc.iommu_clock.id){enum a733_clock_result r=a733_clock_acquire(c->clocks,c->owner,A733_CLOCK_FABRIC|A733_CLOCK_REF|A733_CLOCK_OSC,
        A733_CLOCK_IOMMU_SYS|A733_CLOCK_VI|A733_CLOCK_DE_SYS|A733_CLOCK_VE_ENC,hevc_deadline(),&hevc.iommu_clock);if(r==A733_CLOCK_BUSY)return 0;if(r!=A733_CLOCK_OK)return -1;}
    if(!hevc.power_token){enum a733_gpu_resource_result r=a733_gpu_resources_acquire(c->power,c->power_client,A733_GPU_RESOURCE_OSC|A733_GPU_RESOURCE_PCK,hevc_deadline(),&hevc.power_token,&hevc.power_ready);if(r==A733_GPU_RESOURCE_BUSY)return 0;if(r!=A733_GPU_RESOURCE_OK)return -1;}
    c->pck=(struct a733_gpu_resource_lease){hevc.power_token,A733_GPU_RESOURCE_OSC|A733_GPU_RESOURCE_PCK,A733_GPU_RESOURCE_STABLE};
    struct a733_hevc_platform_ops ops={hevc_now,hevc_authority,hevc_memory,hevc_copied,
        hevc_enter,hevc_leave,a733_video_platform_native_read,a733_video_platform_native_write};
    if(!a733_hevc_platform_init(&hevc.platform,c,&ops,&hevc))return -1;
    return a733_hevc_begin(&hevc.run,a733_video_platform_hevc_ops(),&hevc.platform,&hevc.storage.job,hevc_deadline())?1:-1;
}
/* The controller performs at most one register operation per poll. Repeat
 * polls while each one advances its state or index, so programming sequences
 * finish in one wake. Waiting for hardware, a copy step or a busy resource
 * makes no progress and sleeps a tick; the budget bounds one wake's work. */
#define HEVC_POLL_BUDGET 1024u
static int hevc_advance(void)
{
    for(unsigned n=0;;){
        enum a733_hevc_state state=hevc.run.state;unsigned index=hevc.run.index;
        int r=a733_hevc_poll(&hevc.run);
        if(r||(hevc.run.state==state&&hevc.run.index==index)||++n==HEVC_POLL_BUDGET||!board_live(NULL))return r;
    }
}
/* Publish the copied picture, then end after the P picture or program it
 * once the host has acknowledged the IDR copy. */
static int hevc_copied_picture(void)
{
    unsigned picture=hevc.run.picture;
    if(atomic_load_explicit(&hevc.completed,memory_order_acquire)==picture){
        if(!board_live(NULL))return -1;
        atomic_store_explicit(&hevc.completed,picture+1,memory_order_release);
    }
    if(picture){atomic_store_explicit(&hevc.result,1,memory_order_release);return 0;}
    if(atomic_load_explicit(&hevc.acknowledged,memory_order_acquire)<1)return 0;
    return a733_hevc_next(&hevc.run)==1?0:-1;
}
/* Any failure ends the session with every allocation, mapping, lease and
 * token retained until whole-board power removal. */
void hevc_task(void *unused)
{
    (void)unused;
    for(;;){
        if(!board_live(NULL)){atomic_store_explicit(&hevc.result,2,memory_order_release);vTaskDelay(1);continue;}
        if(atomic_load_explicit(&providers.shared.pmic_ready,memory_order_acquire)&&!atomic_load_explicit(&hevc.result,memory_order_acquire)){
            if(!hevc_input_ready(&hevc.supplied)){vTaskDelay(1);continue;}
            int r;
            if(!hevc.storage.published)
                r=hevc_storage_publish(&hevc.storage,hevc.supplied.total,&hevc.supplied.plan,hevc.session.input_digest)?0:-1;
            else r=hevc.stage?hevc_advance():hevc_setup();
            if(r>=0&&!hevc.stage&&r==1)hevc.stage=1;
            else if(r>=0&&hevc.stage&&hevc.run.state==HEVC_COPIED)r=hevc_copied_picture();
            if(r<0)atomic_store_explicit(&hevc.result,2,memory_order_release);
        }
        vTaskDelay(1);
    }
}
