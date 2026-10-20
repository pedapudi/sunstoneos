#include "encode.h"
#include "a733_memory_uncached.h"
#include "a733_memory_reservations.h"
#include "a733_storage_digest.h"
#include "aarch64_device_memory.h"
#include "boot.h"
#include <string.h>
struct h264_encode_application h264_encode;
static uint64_t h264_encode_deadline(void){return board.session.deadline_us-10000;}
static int h264_encode_now(void *v,uint64_t *out)
{return v==&h264_encode&&board_live(NULL)&&a733_counter_clock_now(&providers.shared.counter,out)==1;}
static int h264_encode_enter(void *v,uint64_t d)
{return v==&h264_encode&&board_live(NULL)?a733_platform_lock_enter(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK,d):-1;}
static int h264_encode_leave(void *v)
{if(v!=&h264_encode)return -1;a733_platform_lock_leave(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK);return 1;}
static int h264_encode_device(uintptr_t address)
{
    struct aarch64_translation t;
    return aarch64_device_translate(address,1,&t)&&t.physical==address&&t.attributes==0&&t.shareability==2;
}
static int h264_encode_load(uintptr_t address,uint32_t *value)
{if(!h264_encode_device(address)||!board_live(NULL))return -1;__asm__ volatile("dsb sy":::"memory");*value=*(volatile uint32_t *)address;__asm__ volatile("dsb sy":::"memory");return board_live(NULL)?1:-1;}
static int h264_encode_iommu_read(void *v,uint64_t token,uint32_t off,uint32_t *value,uint64_t d)
{return v==&h264_encode&&token==h264_encode.session.authority&&d==h264_encode_deadline()&&off<0x20000&&!(off&3)?h264_encode_load(A733_IOMMU_BASE+off,value):-1;}
static int h264_encode_iommu_write(void *v,uint64_t token,uint32_t off,uint32_t value,uint64_t d)
{
    uint32_t ignored;
    if(h264_encode_iommu_read(v,token,off,&ignored,d)!=1)return -1;
    if(h264_encode_enter(v,d)!=1)return -1;
    *(volatile uint32_t *)(uintptr_t)(A733_IOMMU_BASE+off)=value;
    __asm__ volatile("dsb sy":::"memory");
    if(!board_live(NULL))return -1;return h264_encode_leave(v);
}
static int h264_encode_publish(void *v,uint64_t token,const void *p,size_t n,uint64_t d)
{
    if(v!=&h264_encode||token!=h264_encode.session.authority||p!=h264_encode.storage.table||n!=32768||d!=h264_encode_deadline())return -1;
    __asm__ volatile("dsb sy":::"memory");return board_live(NULL)?1:-1;
}
static int h264_encode_iommu_admit(void *v,const struct a733_iommu_tables *t,uint64_t d,struct a733_iommu_admission *a)
{
    if(v!=&h264_encode||t->words!=h264_encode.storage.table||d!=h264_encode_deadline()||!board_live(NULL))return -1;
    *a=(struct a733_iommu_admission){.token=h264_encode.session.authority,
        .excluded_masters=A733_IOMMU_REAL_MASTERS,.powered_masters=0x8,
        .inherited_resolved=1,.transactions_drained=1,.clocks_held=1,.retained_power_removal=1};
    /* These facts come from the full authenticated initial-isolation record,
     * never inferred from disabled ENABLE, empty fault registers or CPU fences. */
    return a733_clock_token_status(&providers.shared.clocks,&h264_encode.iommu_clock)==A733_CLOCK_OK?1:-1;
}
static int authority(void *v,const struct a733_encode_platform_config *c,
 const struct a733_h264_encode_job *j,uint64_t d,uint64_t *token)
{
 if(v!=&h264_encode||!token)return -1;
 if(*token&&*token!=h264_encode.session.authority)return -1;*token=h264_encode.session.authority;
 if(!board_live(NULL)||d!=h264_encode_deadline()||memcmp(c,&h264_encode.config,sizeof(*c))||
   !h264_encode_input_ready(&h264_encode.supplied)||!h264_encode.storage.published)return -1;
 for(unsigned i=0;i<2;++i)if(j->input[i]!=h264_encode.storage.job.input[i])return -1;
 for(unsigned i=0;i<10;++i)if(j->buffers[i].iova!=h264_encode.storage.job.buffers[i].iova||j->buffers[i].bytes!=h264_encode.storage.job.buffers[i].bytes)return -1;
 struct storage_digest digest;uint8_t hash[32];storage_digest_init(&digest);
 for(unsigned i=0;i<2;++i)if(!storage_digest_add(&digest,j->input[i],460800))return -1;
 storage_digest_end(&digest,hash);
 if(memcmp(hash,h264_encode.session.input_digest,32)||!h264_encode_device(0x01c10000))return -1;
 uint32_t gate;for(unsigned i=0;i<2;++i)if(h264_encode_load(0x02002000+(i?0x5b4:0x58c),&gate)!=1||
   (gate&0x10007)!=h264_encode.session.iommu_gates[i])return -1;
 return h264_encode_storage_guards(&h264_encode.storage)?1:-1;
}
static int memory(void *v,uint64_t token,const struct a733_h264_encode_job *j,uint64_t d)
{
 if(v!=&h264_encode||token!=h264_encode.session.authority||d!=h264_encode_deadline()||
    j->input[0]!=h264_encode.storage.job.input[0]||!h264_encode.storage.published)return -1;
 if(!h264_encode.iommu.initialized){
  struct a733_iommu_ops ops={h264_encode_now,h264_encode_iommu_admit,h264_encode_iommu_read,h264_encode_iommu_write,h264_encode_publish};
  if(a733_iommu_init_encoder(&h264_encode.iommu,&ops,&h264_encode,&h264_encode.storage.tables)!=1||
    a733_iommu_open(&h264_encode.iommu,d)!=1)return -1;
 }
 return a733_iommu_poll(&h264_encode.iommu);
}
static int finished(size_t end)
{
 uint32_t status,count;return h264_encode_load(0x01c10b1c,&status)==1&&(status&15)==1&&
  h264_encode_load(0x01c10b90,&count)==1&&count%8==0&&count/8==end;
}
static int prime(void *v,uint64_t token,unsigned ordinal,size_t cursor,uint64_t d)
{
 if(v!=&h264_encode||token!=h264_encode.session.authority||d!=h264_encode_deadline()||ordinal>1||
   cursor>2097152||!board_live(NULL)||!h264_encode_storage_guards(&h264_encode.storage))return -1;
 if(ordinal){
  if(!atomic_load_explicit(&h264_encode.acked[0],memory_order_acquire)||
    !atomic_load_explicit(&h264_encode.result[0],memory_order_acquire)||
    cursor!=((h264_encode.run.end[0]+63u)&~(size_t)63u)||!finished(h264_encode.run.end[0]))return -1;
 }else if(cursor)return -1;
 if(h264_encode.prime_step[ordinal]==2)return 1;
 if(!h264_encode.prime_step[ordinal]){if(a733_iommu_scan(&h264_encode.iommu,d)!=1)return -1;h264_encode.prime_step[ordinal]=1;}
 int r=a733_iommu_poll(&h264_encode.iommu);if(r!=1)return r;
 if(h264_encode.iommu.fault.scanned_masters!=8||!h264_encode_storage_guards(&h264_encode.storage)||
   (ordinal&&!finished(h264_encode.run.end[0])))return -1;
 size_t n;const uint8_t *prefix=a733_h264_encode_prefix(ordinal,&n);
 if(!prefix||cursor+n>4194304)return -1;
 memcpy(h264_encode.storage.buffers[A733_H264_ENCODE_RING]+cursor,prefix,n);
 __asm__ volatile("dsb sy":::"memory");h264_encode.prime_step[ordinal]=2;return board_live(NULL)?1:-1;
}
static int copied(void *v,uint64_t token,unsigned ordinal,size_t first,size_t end,uint64_t d)
{
 if(v!=&h264_encode||token!=h264_encode.session.authority||d!=h264_encode_deadline()||ordinal>1||
   first>=end||end>4194304||end-first>1048576-(ordinal?0:22)||!board_live(NULL))return -1;
 size_t prefix=ordinal?0:22,n=end-first+prefix;
 if(h264_encode.output_bytes[ordinal]&&h264_encode.output_bytes[ordinal]!=n)return -1;
 h264_encode.output_bytes[ordinal]=n;
 if(!h264_encode.copy_step[ordinal]){
  if(a733_iommu_scan(&h264_encode.iommu,d)!=1)return -1;
  h264_encode.copy_step[ordinal]=1;
  if(!ordinal)memcpy(h264_encode.storage.copies[0],a733_h264_encode_sps_pps,22);
 }
 int r=a733_iommu_poll(&h264_encode.iommu);if(r!=1)return r;
 if(h264_encode.iommu.fault.scanned_masters!=8||!finished(end)||!h264_encode_storage_guards(&h264_encode.storage))return -1;
 if(h264_encode.copied[ordinal]<end-first){
  size_t done=h264_encode.copied[ordinal],chunk=end-first-done;if(chunk>32768)chunk=32768;
  __asm__ volatile("dsb sy":::"memory");
  memcpy(h264_encode.storage.copies[ordinal]+prefix+done,h264_encode.storage.buffers[A733_H264_ENCODE_RING]+first+done,chunk);
  __asm__ volatile("dsb sy":::"memory");
  if(!finished(end)||!h264_encode_storage_guards(&h264_encode.storage)||!board_live(NULL))return -1;
  h264_encode.copied[ordinal]+=chunk;if(a733_iommu_scan(&h264_encode.iommu,d)!=1)return -1;return 0;
 }
 storage_digest_bytes(h264_encode.storage.copies[ordinal],n,h264_encode.digest[ordinal]);
 return finished(end)&&h264_encode_storage_guards(&h264_encode.storage)?1:-1;
}
int h264_encode_storage_allocate(void)
{
 struct h264_encode_storage *s=&h264_encode.storage;
 if(s->initialized)return 0;
 if(!h264_encode_storage_init(s,a733_memory_reservations_init(),a733_memory_uncached_init(),h264_encode.session.owner))return 0;
 return h264_encode_input_bind(&h264_encode.supplied,s->upload,925696);
}
static int setup(void)
{
    struct a733_encode_platform_config *c=&h264_encode.config;
    if(!c->owner)*c=(struct a733_encode_platform_config){.clocks=&providers.shared.clocks,.power=&providers.shared.gpu,
      .power_client=(unsigned)h264_encode.session.pck_client,.owner=h264_encode.session.owner,
      .handoff=h264_encode.session.board.generation,.module=h264_encode.session.module|0x80000000,
      .module_hz=h264_encode.session.module_hz};
    memcpy(c->fabric,h264_encode.session.board.fabric,sizeof(c->fabric));
    memcpy(c->fabric_hz,h264_encode.session.board.fabric_hz,sizeof(c->fabric_hz));
    enum a733_rail_result rr;
    if(!h264_encode.rail.id)rr=a733_rail_acquire(&providers.shared.rails,c->owner,
      A733_RAIL_BIT(h264_encode.session.supply_rail),0,h264_encode_deadline(),&h264_encode.rail);
    else rr=a733_rail_poll(&providers.shared.rails,&h264_encode.rail);
    if(rr==A733_RAIL_BUSY||rr==A733_RAIL_PENDING)return 0;if(rr!=A733_RAIL_READY)return -1;
    enum a733_pmic_status pr;
    if(!h264_encode.request.id){pr=a733_pmic_submit(&providers.shared.pmic,c->owner,&h264_encode.rail,
      (enum a733_rail)h264_encode.session.supply_rail,h264_encode_deadline(),&h264_encode.request);
      return pr==A733_PMIC_PENDING||pr==A733_PMIC_BUSY?0:-1;}
    pr=a733_pmic_result(&providers.shared.pmic,&h264_encode.request,&h264_encode.supply);
    if(pr==A733_PMIC_PENDING||pr==A733_PMIC_BUSY)return 0;
    if(pr!=A733_PMIC_COMPLETE||h264_encode.supply.rail!=h264_encode.session.supply_rail||
      h264_encode.supply.page_before||h264_encode.supply.page_after||h264_encode.supply.chip!=3||h264_encode.supply.revision||
      !h264_encode.supply.enabled||!h264_encode.supply.decoded||h264_encode.supply.programmed_uv!=h264_encode.session.supply_uv)return -1;
    a733_clock_mask_t scopes;if(!a733_encode_platform_scopes(c,&scopes))return -1;
    if(!c->clock.id){enum a733_clock_result r=a733_clock_acquire(c->clocks,c->owner,scopes,A733_CLOCK_VE_ENC,h264_encode_deadline(),&c->clock);
      if(r==A733_CLOCK_BUSY)return 0;if(r!=A733_CLOCK_OK)return -1;}
    if(!h264_encode.iommu_clock.id){enum a733_clock_result r=a733_clock_acquire(c->clocks,c->owner,A733_CLOCK_FABRIC|A733_CLOCK_REF|A733_CLOCK_OSC,
      A733_CLOCK_IOMMU_SYS|A733_CLOCK_VI|A733_CLOCK_DE_SYS|A733_CLOCK_VE_DEC,h264_encode_deadline(),&h264_encode.iommu_clock);
      if(r==A733_CLOCK_BUSY)return 0;if(r!=A733_CLOCK_OK)return -1;}
    if(!h264_encode.power_token){enum a733_gpu_resource_result r=a733_gpu_resources_acquire(c->power,c->power_client,
      A733_GPU_RESOURCE_OSC|A733_GPU_RESOURCE_PCK,h264_encode_deadline(),&h264_encode.power_token,&h264_encode.power_ready);
      if(r==A733_GPU_RESOURCE_BUSY)return 0;if(r!=A733_GPU_RESOURCE_OK)return -1;}
    c->pck=(struct a733_gpu_resource_lease){h264_encode.power_token,A733_GPU_RESOURCE_OSC|A733_GPU_RESOURCE_PCK,A733_GPU_RESOURCE_STABLE};
    struct a733_h264_encode_platform_ops ops={h264_encode_now,authority,memory,prime,copied,h264_encode_enter,h264_encode_leave,a733_encode_native_read,a733_encode_h264_native_write};
    if(!h264_encode_storage_publish(&h264_encode.storage,h264_encode.session.input_digest)||!a733_h264_encode_platform_init(&h264_encode.platform,c,&ops,&h264_encode))return -1;
    return a733_h264_encode_begin(&h264_encode.run,a733_encode_platform_h264_ops(),&h264_encode.platform,&h264_encode.storage.job,h264_encode_deadline())?1:-1;
}
void h264_encode_task(void *unused)
{
 (void)unused;for(;;){
  if(!board_live(NULL)){atomic_store_explicit(&h264_encode.fatal,1,memory_order_release);vTaskDelay(1);continue;}
  if(atomic_load_explicit(&providers.shared.pmic_ready,memory_order_acquire)&&!atomic_load_explicit(&h264_encode.fatal,memory_order_acquire)){
   if(!h264_encode_input_ready(&h264_encode.supplied)){vTaskDelay(1);continue;}
   int r=0;
   if(!h264_encode.stage){r=setup();if(r==1)h264_encode.stage=1;}
   else if(h264_encode.stage==2){
    if(atomic_load_explicit(&h264_encode.acked[0],memory_order_acquire)){
     r=a733_h264_encode_next(&h264_encode.run);if(r==1)h264_encode.stage=3;else r=-1;
    }
   }else if(h264_encode.stage==1||h264_encode.stage==3){
    r=a733_h264_encode_poll(&h264_encode.run);
    if(r==1&&h264_encode.run.state==A733_H264_ENCODE_COPIED){
     unsigned ordinal=h264_encode.run.ordinal;
     atomic_store_explicit(&h264_encode.result[ordinal],1,memory_order_release);
     h264_encode.stage=ordinal?4:2;
    }
   }
   if(r<0)atomic_store_explicit(&h264_encode.fatal,1,memory_order_release);
  }vTaskDelay(1);
 }
}
