#include "uart.h"
#include "boot.h"
#include "a733_storage_digest.h"
#include <string.h>
struct uart_dma_application uart_dma;
static int parent(void)
{
 struct uart_dma_application *d=&uart_dma;
 if(!d->pool || !d->pool->initialized || d->extent.bytes!=16384 ||
 d->extent.cpu!=d->extent.physical)return 0;
 struct device_memory_allocation *a=device_memory_lookup(d->pool,d->lease);
 return a && !memcmp(a,&d->allocation,sizeof(*a)) && a->state==DEVICE_MEMORY_DEVICE;
}
static _Noreturn void fatal(void *v)
{(void)v;atomic_store_explicit(&uart.failed,1,memory_order_release);board_fail(NULL);for(;;)vTaskDelay(1);}
static int worker(void)
{return parent()&&board_live(NULL)&&
 !atomic_load_explicit(&providers.shared.clocks.failed,memory_order_acquire)&&
 !atomic_load_explicit(&providers.shared.locks.retained,memory_order_acquire)&&a733_dma_startup_ready(&uart_dma.startup)&&
 xTaskGetSchedulerState()==taskSCHEDULER_RUNNING&&
 xTaskGetCurrentTaskHandle()==uart_dma.service.worker;}
static uint32_t read_register(void *v,uintptr_t a)
{if(v!=&uart_dma||!worker())fatal(v);return a733_dma_native_operations.read(&uart_dma.native,a);}
static void write_register(void *v,uintptr_t a,uint32_t n)
{if(v!=&uart_dma||!worker())fatal(v);a733_dma_native_operations.write(&uart_dma.native,a,n);}
static void cache(void *v,enum a733_dma_cache op,void *p,size_t n)
{
 if(v!=&uart_dma||!worker()||n!=4096||(uintptr_t)p<uart_dma.extent.cpu||
 (uintptr_t)p-uart_dma.extent.cpu>=16384||((uintptr_t)p&4095))fatal(v);
 a733_dma_native_operations.cache(&uart_dma.native,op,p,n);
}
static void barrier(void *v)
{if(v!=&uart_dma||!worker())fatal(v);a733_dma_native_operations.barrier(&uart_dma.native);}
static uint64_t ticks(void *v)
{if(v!=&uart_dma||!worker())fatal(v);return a733_dma_native_operations.ticks(&uart_dma.native);}
static int mapping(void *v,const struct a733_dma_mapping *m)
{
 if(v!=&uart_dma||!parent()||!m)return 0;
 if(xTaskGetSchedulerState()==taskSCHEDULER_RUNNING&&!worker())return 0;
 uintptr_t p=(uintptr_t)m->cpu;
 if(p<uart_dma.extent.cpu || p-uart_dma.extent.cpu>=16384 ||
 (p&4095)||m->bytes!=4096||m->bus!=p)return 0;
 unsigned page=(p-uart_dma.extent.cpu)/4096;
 enum a733_dma_access role=page<2?A733_DMA_DESCRIPTORS:page==2?A733_DMA_WRITE:A733_DMA_READ;
 return m->access==role&&a733_dma_native_operations.validate_mapping(&uart_dma.native,m);
}
static int fifo(void *v,const struct a733_dma_endpoint *e,enum a733_dma_access role)
{
 if(v!=&uart_dma||!parent()||!e)return 0;
 if(xTaskGetSchedulerState()==taskSCHEDULER_RUNNING&&!worker())return 0;
 return e->address==0x02502000&&e->request==16&&e->width==1&&e->burst==1&&
 a733_dma_native_operations.validate_fifo(&uart_dma.native,e,role);
}
static const struct a733_dma_operations operations={read_register,write_register,cache,barrier,ticks,mapping,fifo};
static int prepared(void *v,uintptr_t base,uint16_t channels)
{return v==&uart_dma&&parent()&&uart_live()?a733_dma_startup_prepared(&uart_dma.startup,base,channels):-1;}
int uart_dma_construct(void)
{
 struct uart_dma_application *d=&uart_dma;
 const struct uart_dma_authority *w=&uart.session.dma;
 uint64_t start=board.session.session_start_ticks/24;
 if(d->pool || start>UINT64_MAX-w->startup_timeout_us)return 0;
 struct a733_dma_handoff h={.allowed={.owner=w->owner,.handoff=w->handoff,
 .deadline_us=start+w->startup_timeout_us,.cold=w->cold,
 .expected_cpu=w->expected_cpu,.expected_mcu=w->expected_mcu,
 .desired_cpu=w->desired_cpu,.desired_mcu=w->desired_mcu,.route_change_mask=w->route_change_mask},
 .controller=w->controller,.evidence=w->evidence,.established=w->established,
 .owned_channels=w->owned_channels,.service_channels=w->service_channels};
 memcpy(h.allowed.fabric,w->fabric,sizeof(w->fabric));memcpy(h.allowed.rate_hz,w->rate_hz,sizeof(w->rate_hz));
 if(!a733_dma_startup_supervised_init(&d->startup,&h,&providers.shared.counter,
 &providers.shared.clocks,&providers.shared.locks)||!a733_dma_startup_bind_task(&d->startup,uart.handle))return 0;
 d->pool=a733_memory_reservations_init();
 if(!d->pool||!device_memory_acquire(d->pool,w->owner,16384,&d->lease,&d->extent))return 0;
 struct device_memory_allocation *a=device_memory_lookup(d->pool,d->lease);
 if(!a)return 0;a->state=DEVICE_MEMORY_DEVICE;d->allocation=*a;
 struct dma_memory_window windows[4];
 for(unsigned i=0;i<4;++i)windows[i]=(struct dma_memory_window){d->extent.cpu+4096*i,d->extent.physical+4096*i,4096,
 DMA_MEMORY_ROLE(i<2?A733_DMA_DESCRIPTORS:i==2?A733_DMA_WRITE:A733_DMA_READ)};
 const struct dma_fifo_port ports[]={{0x02502000,16,2,2,A733_DMA_READ},{0x02502000,16,2,2,A733_DMA_WRITE}};
 if(!parent()||!a733_dma_native_init(&d->native,0x04601000,0x04601000,windows,4,ports,2,fatal,d))return 0;
 memset((void *)d->extent.cpu,0,16384);
 memcpy((void *)(d->extent.cpu+12288),uart.tx,uart.session.tx_bytes);
 for(unsigned j=0;j<2;++j){
  struct a733_dma_endpoint ram={d->extent.physical+(j?12288:8192),0,1,1},port={0x02502000,16,1,1};
  d->segments[j]=(struct a733_dma_segment){j?ram:port,j?port:ram,uart.session.tx_bytes};
  d->mappings[j]=(struct a733_dma_mapping){(void *)(d->extent.cpu+(j?12288:8192)),ram.address,4096,j?A733_DMA_READ:A733_DMA_WRITE};
  d->jobs[j]=(struct dma_service_job){.kind=DMA_SERVICE_FIFO,.channel=j,
   .transfer={{(void *)(d->extent.cpu+4096*j),d->extent.physical+4096*j,4096,A733_DMA_DESCRIPTORS},&d->segments[j],1,&d->mappings[j],1}};
  if(!mapping(d,&d->jobs[j].transfer.descriptors)||!mapping(d,&d->mappings[j]))return 0;
 }
 /* All privileged image/stack/session state outside this exact reservation is
  * excluded. Native policy admits only these four immutable whole pages. */
 const struct dma_service_protected protected[]={
 {0x41000000,0x41000000,d->extent.cpu-0x41000000},
 {d->extent.cpu+16384,d->extent.physical+16384,0x42200000-d->extent.cpu-16384}};
 const struct dma_service_activation activation={uart.handle,prepared,d};
 return dma_service_init_deferred(&d->service,&operations,d,0x04601000,3,2400000,
 d->jobs,2,protected,2,5,1,&activation)&&dma_service_bind(&d->service,0,uart.handle,3);
}
/* Exactly one outstanding copied service reply per UART owner. A collection
 * timeout preserves its ticket and operation; it never reissues SUBMIT. */
static int rpc(unsigned job,enum dma_service_operation op,struct dma_service_reply *reply)
{
 struct uart_dma_application *d=&uart_dma;
 if(!d->pending){
  struct dma_service_request r={.operation=op,.job=job,
   .timeout=op==DMA_SERVICE_SUBMIT?uart.session.operation_timeout_us*24:0};
  if(!dma_service_send(&d->service,&r,0,&d->pending))return 0;
  d->operation=op;d->job=job;
 }
 if(d->operation!=(unsigned)op||d->job!=job)return -1;
 if(!dma_service_collect(&d->service,d->pending,reply,0))return 0;
 uint64_t ticket=d->pending;d->pending=0;
 if(reply->ticket!=ticket||reply->result!=DMA_SERVICE_OK)return -1;
 if(op==DMA_SERVICE_SUBMIT){
  if(reply->job_ticket!=ticket||!reply->holds_buffers||!reply->owns_session)return -1;
  d->job_tickets[job]=ticket;
 }
 if(op==DMA_SERVICE_STATUS && (reply->job_ticket!=d->job_tickets[job]||
 !reply->holds_buffers||!reply->owns_session))return -1;
 return 1;
}
static int retained_failure(void)
{
 if(uart.platform.initialized)(void)a733_uart_platform_retain(&uart.platform);
 atomic_store_explicit(&uart.failed,1,memory_order_release);
 atomic_store_explicit(&uart.result,2,memory_order_release);return -1;
}
static int failure(void)
{
 struct uart_dma_application *d=&uart_dma;
 if(d->failing)return 0;
 d->failing=1;
 /* A separate bounded cleanup interval permits request gating/cancellation
  * after the operation deadline. It never authorizes additional data or reuse.
  * Authority loss skips MMIO and retains an explicitly uncertain stop. */
 uint64_t now=a733_smp_counter()/24,cap=board.session.deadline_us-10000;
 if(!uart_live()||!d->activated){d->uncertain=1;return retained_failure();}
 d->stop_deadline=cap-now>100000?now+100000:cap;
 if(uart.platform.uart.state==A733_UART_READY&&uart.platform.uart.dma_owned){
  if(a733_uart_dma_requests(&uart.platform.uart,0,d->stop_deadline)==A733_UART_READY)d->requests_disabled=1;
  else d->uncertain=1;
 }else if(uart.platform.uart.state==A733_UART_FAULT)d->uncertain=1;
 return 0;
}
static int cancel_failure(void)
{
 struct uart_dma_application *d=&uart_dma;
 if(!uart_live()||a733_smp_counter()/24>=d->stop_deadline){d->uncertain=1;return retained_failure();}
 struct dma_service_reply reply;
 if(d->pending){
  if(!dma_service_collect(&d->service,d->pending,&reply,0))return 0;
  if(reply.ticket!=d->pending){d->uncertain=1;return retained_failure();}
  d->pending=0;
  if(d->cancel_operation==1){
   if(reply.result!=DMA_SERVICE_OK||reply.phase==DMA_SERVICE_RECOVERY){d->uncertain=1;++d->cancel_job;}
   else if(reply.phase==DMA_SERVICE_RUNNING)d->cancel_operation=2;
   else if(reply.phase==DMA_SERVICE_DRAINING)d->cancel_operation=0;
   else {++d->cancel_job;d->cancel_operation=0;}
  }else d->cancel_operation=0; /* normal reply or CANCEL acknowledgment */
 }
 if(d->cancel_job==2)return retained_failure();
 struct dma_service_request request={.operation=d->cancel_operation==2?DMA_SERVICE_CANCEL:DMA_SERVICE_STATUS,.job=d->cancel_job};
 if(!dma_service_send(&d->service,&request,0,&d->pending))return 0;
 d->cancel_operation=request.operation==DMA_SERVICE_STATUS?1:3;return 0;
}
static int publish(void)
{
 struct uart_dma_application *d=&uart_dma;
 memset(uart.payload,0,sizeof(uart.payload));
 uint64_t observed=a733_smp_counter()/24;memcpy(uart.payload,&observed,8);
 uint32_t fields[]={uart.platform.uart.plan.actual_baud,2,(uint32_t)uart.sent,
  (uint32_t)uart.received,uart.platform.uart.errors,A733_UART_PLATFORM_CLOSED};
 memcpy(uart.payload+8,fields,sizeof(fields));
 memcpy(uart.payload+32,d->job_tickets,sizeof(d->job_tickets));
 uint32_t completion[]={DMA_SERVICE_COMPLETE,DMA_SERVICE_COMPLETE,d->closed,
  d->requests_disabled,d->drained,d->polls[0],d->polls[1],1};
 memcpy(uart.payload+48,completion,sizeof(completion));
 memcpy(uart.payload+96,uart.tx,uart.sent);memcpy(uart.payload+96+uart.sent,uart.rx,uart.received);
 uart.payload_bytes=96+uart.sent+uart.received;
 storage_digest_bytes(uart.payload,uart.payload_bytes,uart.digest);
 if(!uart_live()||observed>=uart.deadline)return failure();
 atomic_store_explicit(&uart.result,1,memory_order_release);return 1;
}
int uart_dma_step(void)
{
 struct uart_dma_application *d=&uart_dma;
 if(atomic_load_explicit(&uart.failed,memory_order_acquire)){
  if(uart.platform.initialized)(void)a733_uart_platform_retain(&uart.platform);
  return -1;
 }
 if(d->failing)return cancel_failure();
 if(!uart_live())return failure();
 if(atomic_load_explicit(&uart.result,memory_order_acquire)==1)return 1;
 if(a733_smp_counter()/24>=uart.deadline)return failure();
 struct a733_uart *u=&uart.platform.uart;
 enum a733_uart_platform_result p;
 struct dma_service_reply reply;
 int r;
 switch(uart.stage){
 case 0:
  p=a733_uart_platform_poll(&uart.platform);
  if(p==A733_UART_PLATFORM_PENDING||p==A733_UART_PLATFORM_BUSY)return 0;
  if(p!=A733_UART_PLATFORM_READY||a733_uart_dma_prepare(u,uart.deadline)!=A733_UART_READY)return failure();
  uart.stage=100;return 0;
 case 100: {
  enum a733_dma_platform_result s=a733_dma_startup_poll(&d->startup);
  if(s==A733_DMA_PLATFORM_PENDING||s==A733_DMA_PLATFORM_BUSY)return 0;
  if(s!=A733_DMA_PLATFORM_READY)return failure();
  enum dma_service_result a=dma_service_activate(&d->service);
  if(a==DMA_SERVICE_BUSY)return 0;
  if(a!=DMA_SERVICE_OK)return failure();d->activated=1;uart.stage=101;return 0;
 }
 case 101:case 102:
  r=rpc(uart.stage-101,DMA_SERVICE_OPEN,&reply);if(!r)return 0;
  if(r<0||reply.phase!=DMA_SERVICE_READY||!reply.owns_session||reply.holds_buffers)return failure();
  ++uart.stage;return 0;
 case 103:case 104:
  r=rpc(uart.stage-103,DMA_SERVICE_SUBMIT,&reply);if(!r)return 0;
  /* With requests disabled no byte may complete. Require both channels ACTIVE
   * before exposing UART requests, RX first to avoid a FIFO overrun. */
  if(r<0||reply.phase!=DMA_SERVICE_RUNNING)return failure();
  ++uart.stage;return 0;
 case 105:
  if(a733_uart_dma_requests(u,1,uart.deadline)!=A733_UART_READY)return failure();
  uart.stage=106;d->active=0;return 0;
 case 106:
  if(a733_uart_dma_status(u,uart.deadline)!=A733_UART_READY||u->errors)return failure();
  r=rpc(d->active,DMA_SERVICE_STATUS,&reply);if(!r)return 0;
  if(r<0||reply.phase==DMA_SERVICE_RECOVERY||
   (reply.phase==DMA_SERVICE_RESULT&&reply.outcome!=DMA_SERVICE_COMPLETE))return failure();
  ++d->polls[d->active];d->replies[d->active]=reply;
  if(d->replies[0].phase==DMA_SERVICE_RESULT&&d->replies[1].phase==DMA_SERVICE_RESULT){uart.stage=107;return 0;}
  d->active^=1;return 0;
 case 107:
  if(a733_uart_dma_requests(u,0,uart.deadline)!=A733_UART_READY)return failure();
  d->requests_disabled=1;uart.stage=108;return 0;
 case 108:
  r=a733_uart_dma_release(u,1,uart.deadline);if(!r)return 0;
  if(r!=A733_UART_READY||u->errors)return failure();d->drained=1;
  /* Both exact terminal tickets still hold the whole allocations. Controller
   * success has invalidated RX cache lines before reporting RESULT. */
  memcpy(uart.rx,(void *)(d->extent.cpu+8192),uart.session.rx_bytes);
  uart.sent=uart.session.tx_bytes;uart.received=uart.session.rx_bytes;
  if(memcmp(uart.tx,uart.rx,uart.sent))return failure();uart.stage=109;return 0;
 case 109:case 110:
  r=rpc(uart.stage-109,DMA_SERVICE_REAP,&reply);if(!r)return 0;
  if(r<0||reply.holds_buffers||reply.phase!=DMA_SERVICE_READY)return failure();
  ++uart.stage;return 0;
 case 111:case 112:
  r=rpc(uart.stage-111,DMA_SERVICE_CLOSE,&reply);if(!r)return 0;
  if(r<0||reply.holds_buffers||reply.owns_session||reply.phase!=DMA_SERVICE_CLOSED)return failure();
  d->closed|=1u<<(uart.stage-111);++uart.stage;return 0;
 case 113:
  uart.stage=3;p=a733_uart_platform_close(&uart.platform,1,uart.deadline);
  if(p==A733_UART_PLATFORM_BUSY){uart.stage=113;return 0;}
  if(p!=A733_UART_PLATFORM_PENDING)return failure();return 0;
 case 3:
  p=a733_uart_platform_poll(&uart.platform);
  if(p==A733_UART_PLATFORM_PENDING||p==A733_UART_PLATFORM_BUSY)return 0;
  if(p!=A733_UART_PLATFORM_CLOSED)return failure();uart.stage=4;return publish();
 default:return failure();
 }
}
int uart_step(void){return uart_dma_step();}
void uart_task(void *v){(void)v;for(;;){uart_step();vTaskDelay(1);}}
