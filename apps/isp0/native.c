#include "isp0.h"
#include "a733_storage_digest.h"
#include "aarch64_device_memory.h"
#include "boot.h"
#include <string.h>
struct isp0_application isp0;
static uint64_t deadline(void){return board.session.deadline_us-10000;}
static int now(void *v,uint64_t *out)
{return v==&isp0&&board_live(NULL)&&a733_counter_clock_now(&providers.shared.counter,out)==1;}
static int live(void)
{uint64_t t;if(now(&isp0,&t)!=1||t<isp0.last_now||t>=deadline())return 0;isp0.last_now=t;return 1;}
static int owner(void)
{
 uint64_t el;__asm__ volatile("mrs %0, CurrentEL":"=r"(el));
 return el==4&&xTaskGetSchedulerState()==taskSCHEDULER_RUNNING&&xTaskGetCurrentTaskHandle()==isp0.handle&&live();
}
static int mapped(uintptr_t a)
{struct aarch64_translation t;return aarch64_device_translate(a,1,&t)&&t.physical==a&&!t.attributes&&t.shareability==2;}
static int load(uintptr_t a,uint32_t *v)
{
 if(!v||!owner()||!mapped(a)||(a&3))return 0;
 __asm__ volatile("dsb sy":::"memory");*v=*(volatile const uint32_t *)a;
 __asm__ volatile("dsb sy":::"memory");return live();
}
static int store(uintptr_t a,uint32_t v)
{
 if(!owner()||!mapped(a)||(a&3))return 0;
 *(volatile uint32_t *)a=v;__asm__ volatile("dsb sy":::"memory");return live();
}
static int enter(void *v,uint64_t d)
{return v==&isp0&&d==deadline()&&owner()?a733_platform_lock_enter(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK,d):-1;}
static void leave(void *v)
{if(v==&isp0)a733_platform_lock_leave(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK);}
static void retain(void *v)
{if(v==&isp0)(void)a733_platform_lock_retain(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK);}
static int media_read(void *v,uintptr_t a,uint32_t *out)
{return v==&isp0&&owner()&&mapped(a)&&a733_media_clock_native_read(NULL,a,out)==1&&live();}
static int media_write(void *v,uintptr_t a,uint32_t value)
{return v==&isp0&&owner()&&mapped(a)&&a733_media_clock_native_write(NULL,a,value)==1&&live();}
static int media_admit(void *v,const struct a733_media_clock_config *c,
 const struct a733_clock_token *token,const struct a733_gpu_resource_lease *power)
{
 return v==&isp0&&owner()&&c==&isp0.media.config&&c->owner==isp0.session.owner&&
  c->handoff==isp0.session.authority&&c->groups==(A733_MEDIA_VI|A733_MEDIA_IOMMU)&&
  !c->camera_mclk_mask&&c->deadline_us==deadline()&&token==&isp0.media.token&&
  token->id&&token->owner==isp0.session.owner&&power==&isp0.media.pck&&power->token==isp0.power_token&&
  !memcmp(c->fabric,isp0.session.board.fabric,sizeof(c->fabric))&&
  !memcmp(c->rate_hz,isp0.session.board.fabric_hz,sizeof(c->rate_hz));
 /* Initial powered-domain, voltage/operating-point, global exclusion and
  * transaction-drain authority is supplied by the sealed entry records. */
}
static int iommu_read(void *v,uint64_t token,uint32_t off,uint32_t *out,uint64_t d)
{return v==&isp0&&token==isp0.session.authority&&d==deadline()&&off<0x20000&&!(off&3)&&load(A733_IOMMU_BASE+off,out)?1:-1;}
static int iommu_write(void *v,uint64_t token,uint32_t off,uint32_t value,uint64_t d)
{
 uint32_t ignored;if(iommu_read(v,token,off,&ignored,d)!=1||enter(v,d)!=1)return -1;
 if(!store(A733_IOMMU_BASE+off,value))return -1;leave(v);return 1;
}
static int publish(void *v,uint64_t token,const void *p,size_t n,uint64_t d)
{
 if(v!=&isp0||token!=isp0.session.authority||p!=isp0.memory[ISP0_TABLE]||n!=32768||d!=deadline())return -1;
 __asm__ volatile("dsb sy":::"memory");return owner()?1:-1;
}
static int iommu_admit(void *v,const struct a733_iommu_tables *t,uint64_t d,struct a733_iommu_admission *a)
{
 if(v!=&isp0||t->words!=(void *)isp0.memory[ISP0_TABLE]||d!=deadline()||!owner()||
    a733_clock_token_status(&providers.shared.clocks,&isp0.media.token)!=A733_CLOCK_OK)return -1;
 *a=(struct a733_iommu_admission){.token=isp0.session.authority,
   .excluded_masters=A733_IOMMU_REAL_MASTERS,.powered_masters=6,.inherited_resolved=1,
   .transactions_drained=1,.clocks_held=1,.retained_power_removal=1};return 1;
}
static int engine_address(uintptr_t a,unsigned writing)
{
 if(a>=0x05800000&&a<=0x05800018)return a==0x05800000||a==0x05800004||a==0x05800008||a==0x0580000c;
 if(a>=0x05800800&&a<=0x058008a0){unsigned o=(unsigned)(a-0x05800800);
  return o==0||o==4||o==8||o==0x20||o==0x24||o==0x28||o==0x30||o==0xa0;}
 if(a>=0x05900000&&a<=0x059000b8){unsigned o=(unsigned)(a-0x05900000);
  if(!writing&&(o==0x50||o==0x80||o==0xb0||o==0x58||o==0x88||o==0xb8))return 1;
  return o==0||o==0x20||o==0x24||o==0x28||o==0x2c||o==0x3c||o==0x40;}
 if(a>=0x05910000&&a<=0x05910100){unsigned o=(unsigned)(a-0x05910000);
  return o==0||o==0x30||o==0x40||o==0x50||(!writing&&(o==0x20||o==0x24||o==0x80||o==0xc0||o==0x100));}
 return 0;
}
static int engine_read(void *v,uintptr_t a,uint32_t *out)
{return v==&isp0&&engine_address(a,0)&&load(a,out)?1:-1;}
static int engine_write(void *v,uintptr_t a,uint32_t value)
{
 if(v!=&isp0||!engine_address(a,1)||enter(v,deadline())!=1)return -1;
 if(!store(a,value))return -1;leave(v);return 1;
}
static int dma_read(void *v,uint64_t token,uint16_t off,uint32_t *out,uint64_t d)
{return v==&isp0&&token==isp0.session.authority&&d==deadline()&&off<0x1000&&!(off&3)&&load(0x05830000u+off,out)?1:-1;}
static int dma_write(void *v,uint64_t token,uint16_t off,uint32_t value,uint64_t d)
{
 uint32_t ignored;if(dma_read(v,token,off,&ignored,d)!=1||enter(v,d)!=1)return -1;
 if(!store(0x05830000u+off,value))return -1;leave(v);return 1;
}
static int dma_admit(void *v,const struct a733_camera_dma_config *c,uint64_t d,uint64_t *token)
{
 if(v!=&isp0||!token||(*token&&*token!=isp0.session.authority)||d!=deadline()||!owner()||
    !isp0.engine.armed||isp0.iommu.state!=A733_IOMMU_ACTIVE||c->block||c->frame_id!=isp0.session.authority||
    c->y.bus_address!=A733_ISP0_Y_IOVA||c->uv.bus_address!=A733_ISP0_UV_IOVA||
    c->y.mapped_bytes!=A733_ISP0_PLANE_BYTES||c->uv.mapped_bytes!=A733_ISP0_PLANE_BYTES||!isp0_guards())return -1;
 *token=isp0.session.authority;return 1;
}
static int start(void *v,uint64_t token,uint64_t d)
{return v==&isp0&&token==isp0.session.authority&&d==deadline()?a733_isp0_trigger(&isp0.engine):-1;}
static int fence(void *v,uint64_t token,uint64_t d)
{return v==&isp0&&token==isp0.session.authority&&d==deadline()?a733_isp0_complete(&isp0.engine):-1;}
static int never_release(void *v,uint64_t token,uint64_t d)
{(void)v;(void)token;(void)d;return -1;}
static int sync_cpu(void *v,uint64_t token,uint64_t d)
{
 if(fence(v,token,d)!=1||!isp0_guards())return -1;
 __asm__ volatile("dsb sy":::"memory");return owner()?1:-1;
}
static int setup(void)
{
 enum a733_rail_result rr;
 if(!isp0.rail.id)rr=a733_rail_acquire(&providers.shared.rails,isp0.session.owner,
   A733_RAIL_BIT(A733_RAIL_DCDC2),0,deadline(),&isp0.rail);
 else rr=a733_rail_poll(&providers.shared.rails,&isp0.rail);
 if(rr==A733_RAIL_BUSY||rr==A733_RAIL_PENDING)return 0;if(rr!=A733_RAIL_READY)return -1;
 enum a733_pmic_status pr;
 if(!isp0.request.id){pr=a733_pmic_submit(&providers.shared.pmic,isp0.session.owner,&isp0.rail,
   A733_RAIL_DCDC2,deadline(),&isp0.request);return pr==A733_PMIC_PENDING||pr==A733_PMIC_BUSY?0:-1;}
 pr=a733_pmic_result(&providers.shared.pmic,&isp0.request,&isp0.supply);
 if(pr==A733_PMIC_PENDING||pr==A733_PMIC_BUSY)return 0;
 if(pr!=A733_PMIC_COMPLETE||isp0.supply.rail!=A733_RAIL_DCDC2||isp0.supply.page_before||
   isp0.supply.page_after||isp0.supply.chip!=3||isp0.supply.revision||!isp0.supply.enabled||
   !isp0.supply.decoded||isp0.supply.programmed_uv!=800000||!(isp0.supply.enable&2)||(isp0.supply.selector&127)!=30)return -1;
 if(!isp0.power_token){enum a733_gpu_resource_result r=a733_gpu_resources_acquire(&providers.shared.gpu,
   (unsigned)isp0.session.pck_client,A733_GPU_RESOURCE_OSC|A733_GPU_RESOURCE_PCK,deadline(),
   &isp0.power_token,&isp0.power_ready);if(r==A733_GPU_RESOURCE_BUSY)return 0;if(r!=A733_GPU_RESOURCE_OK)return -1;
   isp0.power=(struct a733_gpu_resource_lease){isp0.power_token,A733_GPU_RESOURCE_OSC|A733_GPU_RESOURCE_PCK,A733_GPU_RESOURCE_STABLE};}
 if(!isp0.media.initialized){
  uint32_t gate;
  for(unsigned i=0;i<2;i++)if(!load(0x02002000u+(i?0x5b4:0x58c),&gate)||
    (gate&0x10007)!=isp0.session.iommu_gates[i])return -1;
  struct a733_media_clock_config c={.owner=isp0.session.owner,.handoff=isp0.session.authority,
   .deadline_us=deadline(),.groups=A733_MEDIA_VI|A733_MEDIA_IOMMU};
  memcpy(c.fabric,isp0.session.board.fabric,sizeof(c.fabric));memcpy(c.rate_hz,isp0.session.board.fabric_hz,sizeof(c.rate_hz));
  struct a733_media_clock_ops ops={now,media_admit,enter,leave,retain,media_read,media_write};
  if(!a733_media_clock_init(&isp0.media,&c,&ops,&isp0,&providers.shared.clocks,&providers.shared.gpu,
    (unsigned)isp0.session.pck_client,&isp0.power))return -1;
 }
 enum a733_media_clock_result mr=a733_media_clock_poll(&isp0.media);
 if(mr==A733_MEDIA_PENDING||mr==A733_MEDIA_BUSY)return 0;if(mr!=A733_MEDIA_PREPARED)return -1;
 struct a733_iommu_ops ops={now,iommu_admit,iommu_read,iommu_write,publish};
 if(!isp0_allocate()||a733_iommu_init_isp0(&isp0.iommu,&ops,&isp0,&isp0.tables)!=1||
   a733_iommu_open(&isp0.iommu,deadline())!=1)return -1;
 return 1;
}
static int settled(void)
{
 uint32_t status;return live()&&a733_isp0_complete(&isp0.engine)==1&&
  load(0x05830254,&status)&&(status&1)&&!(status&~0xa3u)&&status==isp0.dma.events;
}
static int stable(void){return settled()&&isp0_guards();}
static int step(void)
{
 int r;
 if(!isp0.stage){r=setup();if(r==1)isp0.stage=1;return r<0?-1:0;}
 if(isp0.stage==1){r=a733_iommu_poll(&isp0.iommu);if(r!=1)return r;
  struct a733_isp0_ops ops={now,engine_read,engine_write};
  if(!a733_isp0_init(&isp0.engine,&ops,&isp0,deadline()))return -1;isp0.stage=2;return 0;}
 if(isp0.stage==2){r=a733_isp0_poll(&isp0.engine);if(r!=1)return r;
  struct a733_camera_dma_ops ops={now,dma_admit,dma_read,dma_write,start,fence,never_release,sync_cpu,never_release,NULL,NULL};
  struct a733_camera_dma_config c={.y={A733_ISP0_Y_IOVA,A733_ISP0_PLANE_BYTES},
    .uv={A733_ISP0_UV_IOVA,A733_ISP0_PLANE_BYTES},.frame_id=isp0.session.authority,.block=0};
  if(a733_camera_dma_init(&isp0.dma,&ops,&isp0,&c)!=1||a733_camera_dma_open(&isp0.dma,deadline())!=1)return -1;
  isp0.stage=3;return 0;}
 if(isp0.stage==3){r=a733_camera_dma_poll(&isp0.dma);if(r!=1)return r;
  if(a733_camera_dma_capture(&isp0.dma,deadline())!=1)return -1;isp0.stage=4;return 0;}
 if(isp0.stage==4){
  if(isp0.dma.state==A733_CAM_DMA_STOP&&!isp0.dma.retained_readout&&a733_camera_dma_readout(&isp0.dma,deadline())!=1)return -1;
  r=a733_camera_dma_poll(&isp0.dma);if(r!=1)return r;
  if(isp0.dma.state!=A733_CAM_DMA_READABLE||isp0.dma.owner!=A733_CAMERA_DEVICE||!stable()||
     a733_iommu_scan(&isp0.iommu,deadline())!=1)return -1;isp0.stage=5;return 0;}
 if(isp0.stage==5){r=a733_iommu_poll(&isp0.iommu);if(r!=1)return r;
  if(isp0.iommu.fault.scanned_masters!=6||!stable())return -1;isp0.stage=6;return 0;}
 /* Guard corruption persists, so one scan before the first chunk and one after
  * the last detect it; completion and DMA status are still checked per chunk. */
 if(isp0.stage==6){
  if(!(isp0.copied?settled():stable()))return -1;
  if(isp0.copied<isp0.bytes){size_t n=isp0.bytes-isp0.copied;if(n>4096)n=4096;
   unsigned plane=isp0.copied>=A733_ISP0_PLANE_BYTES?ISP0_UV:ISP0_Y;
   size_t offset=isp0.copied%A733_ISP0_PLANE_BYTES;
   memcpy(isp0.payload+isp0.copied,isp0.memory[plane]+offset,n);isp0.copied+=n;
   if(!settled())return -1;return 0;}
  storage_digest_bytes(isp0.payload,isp0.bytes,isp0.digest);
  if(!stable()||a733_iommu_scan(&isp0.iommu,deadline())!=1)return -1;isp0.stage=7;return 0;
 }
 if(isp0.stage==7){r=a733_iommu_poll(&isp0.iommu);if(r!=1)return r;
  if(isp0.iommu.fault.scanned_masters!=6||!stable())return -1;
  isp0.complete=isp0.guards_ok=1;return 1;}
 return -1;
}
void isp0_task(void *unused)
{
 (void)unused;for(;;){
  if(!live()){atomic_store_explicit(&isp0.result,2,memory_order_release);board_fail(NULL);}
  else if(!atomic_load_explicit(&isp0.result,memory_order_acquire)&&isp0_input_ready(&isp0.supplied)){
   int r=step();if(r<0){atomic_store_explicit(&isp0.result,2,memory_order_release);board_fail(NULL);}
   else if(r==1){if(!live()){atomic_store_explicit(&isp0.result,2,memory_order_release);board_fail(NULL);}
    else{atomic_store_explicit(&isp0.result,1,memory_order_release);
     if(!live()){atomic_store_explicit(&isp0.result,2,memory_order_release);board_fail(NULL);}}}
  }
  vTaskDelay(1);
 }
}
