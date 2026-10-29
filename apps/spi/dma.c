#include "spi.h"
#include "dma.h"
#include "irq.h"
#include "boot.h"
#include "a733_mmio.h"
#include <string.h>
struct spi_dma_application spi_dma;
/* Immutable linked profile, checked by host and constructor. */
const uint32_t spi_dma_image_profile[5]={3,sizeof(struct spi_session),SPI_OUTPUT_BYTES,4096,3};
static int parent(void)
{
 struct spi_dma_application *d=&spi_dma;
 if(!d->pool||!d->pool->initialized||d->extent.bytes!=16384||d->extent.cpu!=d->extent.physical)return 0;
 struct device_memory_allocation *a=device_memory_lookup(d->pool,d->lease);
 return a&&!memcmp(a,&d->allocation,sizeof(*a))&&a->state==DEVICE_MEMORY_DEVICE;
}
void spi_dma_retain(void)
{
 struct spi_dma_application *d=&spi_dma;d->failed=1;
 if(d->startup.token.id&&!d->retained&&
 a733_clock_retain(&providers.shared.clocks,&d->startup.token)!=A733_CLOCK_BUSY)d->retained=1;
}
static _Noreturn void fatal(void *v)
{
 (void)v;spi_cancel();spi_dma.failed=1;
 /* Callback failure cannot return into the controller's completion path. */
 for(;;){spi_step();vTaskDelay(1);}
}
static int owner(void)
{
 return parent()&&!spi_dma.failed&&spi_live()&&a733_dma_startup_ready(&spi_dma.startup)&&
 !atomic_load_explicit(&providers.shared.counter.failed,memory_order_acquire)&&
 xTaskGetSchedulerState()==taskSCHEDULER_RUNNING&&xTaskGetCurrentTaskHandle()==spi.handle;
}
static void check(void *v)
{
 if(v!=&spi_dma||!owner())fatal(v);
 /* INTID143 stays masked/inactive while channel events are polled. */
 if((a733_mmio_read32_dsb_sy_after(0x03400110)|a733_mmio_read32_dsb_sy_after(0x03400310))&(1u<<15))fatal(v);
 if(!owner())fatal(v);
}
static int address(uintptr_t a,int write)
{
 if(!write&&(a==0x04601030||a==0x04601034||a==0x04601038))return 1;
 if(a<0x04601100||a>=0x04601180||(a&3))return 0;
 switch((a-0x04601100)&63){
 case 0:case 4:case 8:case 0x34:case 0x38:return 1;
 case 0x28:case 0x3c:return !write;
 default:return 0;
 }
}
static uint32_t read_register(void *v,uintptr_t a)
{check(v);if(!address(a,0))fatal(v);uint32_t n=a733_dma_native_operations.read(&spi_dma.native,a);check(v);return n;}
static void write_register(void *v,uintptr_t a,uint32_t n)
{check(v);if(!address(a,1))fatal(v);a733_dma_native_operations.write(&spi_dma.native,a,n);check(v);}
static void cache(void *v,enum a733_dma_cache op,void *p,size_t n)
{
 check(v);uintptr_t a=(uintptr_t)p;
 if(n!=4096||a<spi_dma.extent.cpu||a-spi_dma.extent.cpu>=16384||(a&4095))fatal(v);
 unsigned page=(a-spi_dma.extent.cpu)/4096;
 if(page==2?(op!=A733_DMA_CLEAN_INVALIDATE&&op!=A733_DMA_INVALIDATE):op!=A733_DMA_CLEAN)fatal(v);
 a733_dma_native_operations.cache(&spi_dma.native,op,p,n);check(v);
}
static void barrier(void *v)
{check(v);a733_dma_native_operations.barrier(&spi_dma.native);check(v);}
static uint64_t ticks(void *v)
{check(v);uint64_t t=a733_dma_native_operations.ticks(&spi_dma.native);check(v);return t;}
static int mapping(void *v,const struct a733_dma_mapping *m)
{
 if(v!=&spi_dma||!parent()||!m)return 0;
 if(xTaskGetSchedulerState()==taskSCHEDULER_RUNNING&&!owner())return 0;
 uintptr_t a=(uintptr_t)m->cpu;
 if(a<spi_dma.extent.cpu||a-spi_dma.extent.cpu>=16384||(a&4095)||m->bytes!=4096||m->bus!=a)return 0;
 unsigned page=(a-spi_dma.extent.cpu)/4096;
 return m->access==(page<2?A733_DMA_DESCRIPTORS:page==2?A733_DMA_WRITE:A733_DMA_READ)&&
 a733_dma_native_operations.validate_mapping(&spi_dma.native,m);
}
static int fifo(void *v,const struct a733_dma_endpoint *e,enum a733_dma_access role)
{
 if(v!=&spi_dma||!parent()||!e||!owner())return 0;
 return e->address==(role==A733_DMA_READ?0x02543300u:0x02543200u)&&e->request==26&&e->width==1&&e->burst==1&&
 a733_dma_native_operations.validate_fifo(&spi_dma.native,e,role);
}
static const struct a733_dma_operations operations={read_register,write_register,cache,barrier,ticks,mapping,fifo};
int spi_dma_construct(void)
{
 struct spi_dma_application *d=&spi_dma;const struct spi_dma_authority *w=&spi.session.dma;
 const volatile uint32_t *profile=spi_dma_image_profile;
 if(d->pool||profile[0]!=3||profile[1]!=sizeof(spi.session)||profile[2]!=SPI_OUTPUT_BYTES||profile[3]!=4096||profile[4]!=3)return 0;
 uint64_t end;if(!a733_deadline_after(board.session.session_start_ticks/24,w->startup_timeout_us,&end)||end>spi.deadline)return 0;
 struct a733_dma_handoff h={.allowed={.owner=w->owner,.handoff=w->handoff,.deadline_us=end,.cold=w->cold,
 .expected_cpu=w->expected_cpu,.expected_mcu=w->expected_mcu,.desired_cpu=w->desired_cpu,.desired_mcu=w->desired_mcu,
 .route_change_mask=w->route_change_mask},.controller=w->controller,.evidence=w->evidence,.established=w->established,
 .owned_channels=w->owned_channels,.service_channels=w->service_channels};
 memcpy(h.allowed.fabric,w->fabric,sizeof(w->fabric));memcpy(h.allowed.rate_hz,w->rate_hz,sizeof(w->rate_hz));
 if(!a733_dma_startup_supervised_init(&d->startup,&h,&providers.shared.counter,&providers.shared.clocks,&providers.shared.locks)||
 !a733_dma_startup_bind_task(&d->startup,spi.handle))return 0;
 d->pool=a733_memory_reservations_init();
 if(!d->pool||!device_memory_acquire(d->pool,w->owner,16384,&d->lease,&d->extent))return 0;
 struct device_memory_allocation *a=device_memory_lookup(d->pool,d->lease);if(!a)return 0;
 a->state=DEVICE_MEMORY_DEVICE;d->allocation=*a;
 struct dma_memory_window windows[4];
 for(unsigned i=0;i<4;++i)windows[i]=(struct dma_memory_window){d->extent.cpu+4096*i,d->extent.physical+4096*i,4096,
 DMA_MEMORY_ROLE(i<2?A733_DMA_DESCRIPTORS:i==2?A733_DMA_WRITE:A733_DMA_READ)};
 const struct dma_fifo_port ports[]={{0x02543300,26,2,2,A733_DMA_READ},{0x02543200,26,2,2,A733_DMA_WRITE}};
 if(!parent()||!a733_dma_native_init(&d->native,0x04601000,0x04601000,windows,4,ports,2,fatal,d))return 0;
 memset((void *)d->extent.cpu,0,16384);memcpy((void *)(d->extent.cpu+12288),spi.session.tx,spi.session.tx_bytes);
 for(unsigned j=0;j<2;++j){
  struct a733_dma_endpoint ram={d->extent.physical+(j?12288:8192),0,1,1},port={j?0x02543200:0x02543300,26,1,1};
  d->segments[j]=(struct a733_dma_segment){j?ram:port,j?port:ram,j?spi.session.tx_bytes:4096};
  d->mappings[j]=(struct a733_dma_mapping){(void *)(d->extent.cpu+(j?12288:8192)),ram.address,4096,j?A733_DMA_READ:A733_DMA_WRITE};
  d->transfers[j]=(struct a733_dma_transfer){{(void *)(d->extent.cpu+4096*j),d->extent.physical+4096*j,4096,A733_DMA_DESCRIPTORS},&d->segments[j],1,&d->mappings[j],1};
  if(!mapping(d,&d->transfers[j].descriptors)||!mapping(d,&d->mappings[j]))return 0;
 }
 return 1;
}
int spi_dma_start(void)
{
 struct spi_dma_application *d=&spi_dma;
 if(d->failed||!spi_live()||!parent())return -1;
 if(!d->stage){
  enum a733_dma_platform_result r=a733_dma_startup_poll(&d->startup);
  if(r==A733_DMA_PLATFORM_PENDING||r==A733_DMA_PLATFORM_BUSY)return 0;
  if(r!=A733_DMA_PLATFORM_READY)return -1;
  if(a733_dma_controller_init(&d->controller,&operations,d,0x04601000,3,2400000)!=A733_DMA_CONTROL_OK)return -1;
  d->stage=1;return 0;
 }
 if(d->stage<=2){
  if(a733_dma_claim(&d->controller,d->stage-1)!=A733_DMA_CONTROL_OK)return -1;
  ++d->stage;return 0;
 }
 if(d->stage<=4){
  unsigned j=d->stage-3;uint64_t now=a733_smp_counter()/24;
  if(now>=spi.deadline||spi.platform.spi.dma_requests||spi.platform.spi.state!=A733_SPI_READY)return -1;
  if(a733_dma_submit(&d->controller,j,&d->transfers[j],(spi.deadline-now)*24)!=A733_DMA_ACTIVE)return -1;
  d->submitted|=1u<<j;++d->stage;return 0;
 }
 return d->submitted==3&&owner()?1:-1;
}
int spi_dma_observe(void)
{
 struct spi_dma_application *d=&spi_dma;
 if(d->submitted!=3||d->drained||!owner())return -1;
 for(unsigned j=0;j<2;++j){
  enum a733_dma_control_status r=a733_dma_finite_state(&d->controller,j,&d->transfers[j]);++d->observations[j];
  if(r==A733_DMA_COMPLETE)d->completed|=1u<<j;
  else if(r!=A733_DMA_ACTIVE)return -1;
 }
 return owner()?1:-1;
}
int spi_dma_drain(void)
{
 struct spi_dma_application *d=&spi_dma;
 if(!owner()||spi.platform.spi.dma_requests||spi.platform.spi.state!=A733_SPI_COMPLETE||!spi_irq_notifications())return -1;
 for(unsigned j=0;j<2;++j){
  if(d->drained&(1u<<j))continue;
  if(d->controller.channels[j].status==A733_DMA_ACTIVE){
   enum a733_dma_control_status pending=a733_dma_finite_state(&d->controller,j,&d->transfers[j]);
   if(pending==A733_DMA_ACTIVE)return 0;
   if(pending!=A733_DMA_COMPLETE)return -1;d->completed|=1u<<j;
  }
  enum a733_dma_control_status r=a733_dma_poll(&d->controller,j);
  if(r==A733_DMA_STOPPING||r==A733_DMA_ACTIVE)return 0;
  if(r!=A733_DMA_COMPLETE||a733_dma_owns(&d->controller,&d->transfers[j])||!owner())return -1;
  d->drained|=1u<<j;return 0;
 }
 return d->drained==3&&d->completed==3&&owner()?1:-1;
}
int spi_dma_copy(void)
{
 struct spi_dma_application *d=&spi_dma;
 if(!owner()||d->drained!=3||d->completed!=3||spi.platform.spi.dma_requests||
 a733_dma_owns(&d->controller,&d->transfers[0])||a733_dma_owns(&d->controller,&d->transfers[1]))return 0;
 const uint32_t facts[]={3,26,0,3,A733_DMA_COMPLETE,A733_DMA_COMPLETE,4096,spi.session.tx_bytes};
 memcpy(spi.payload+64,facts,sizeof(facts));memcpy(spi.payload+96,(void *)(d->extent.cpu+8192),4096);
 return owner();
}
