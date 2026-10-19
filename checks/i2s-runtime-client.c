/* Actual native client and USB framing; endpoint transport/service effects modeled. */
#include "../apps/i2s/usb0.c"
struct i2s_application app;
volatile unsigned i2s_check_result;
static uint8_t input[4108],wire[128],memory[4][4096];
static unsigned available_bytes,position,wire_bytes,variant,primed,completed,phase,outcome,put_count,delay_once,expected_stop,close_on_enter;
static struct dma_service_request queued;
static uint64_t sent,tick=1,ticket;
static void check(int ok,unsigned code){if(!ok){i2s_check_result=code;__asm__ volatile("brk #0");for(;;){}}}
uint64_t a733_smp_counter(void){return tick;}
int i2s_live(void *p){(void)p;return !atomic_load(&app.fatal);}
void i2s_fail(void *p){(void)p;atomic_store(&app.fatal,1);}
int i2s_prepare_audio(void){atomic_store(&app.audio_ready,1);return 1;}
void vTaskEnterCritical(void){if(close_on_enter){close_on_enter=0;atomic_store(&app.runtime.closed,1);}}
void vTaskExitCritical(void){}
void vTaskDelay(TickType_t t)
{(void)t;check(expected_stop&&atomic_load(&app.fatal),90);check(atomic_load(&app.runtime.head)==(variant==3?1u:0u),91);i2s_check_result=1;__asm__ volatile("brk #0");for(;;){}}
uint32_t tud_vendor_n_available(uint8_t i){return i?0:available_bytes-position;}
uint32_t tud_vendor_n_read(uint8_t i,void *out,uint32_t n)
{check(!i&&n<=available_bytes-position,92);memcpy(out,input+position,n);position+=n;return n;}
uint32_t tud_vendor_n_write(uint8_t i,const void *p,uint32_t n)
{check(!i&&n<=sizeof(wire)-wire_bytes,93);memcpy(wire+wire_bytes,p,n);wire_bytes+=n;return n;}
uint32_t tud_vendor_n_write_flush(uint8_t i){(void)i;return 0;}
int dma_service_copy_period(struct dma_service *s,unsigned j,uint64_t t,void *o,size_t n)
{(void)s;(void)j;(void)t;(void)o;(void)n;return 0;}
int dma_service_send(struct dma_service *s,const struct dma_service_request *r,TickType_t wait,uint64_t *out)
{(void)s;(void)wait;queued=*r;*out=++sent;return 1;}
int dma_service_collect(struct dma_service *s,uint64_t request,struct dma_service_reply *r,TickType_t wait)
{
 (void)s;(void)request;(void)wait;
 if(!delay_once&&((variant==4&&queued.operation==DMA_SERVICE_STATUS&&app.runtime.held&&phase==DMA_SERVICE_READY)||
    (variant==6&&queued.operation==DMA_SERVICE_PERIOD_GET&&phase==DMA_SERVICE_RUNNING))){delay_once=1;return 0;}
 memset(r,0,sizeof(*r));r->result=DMA_SERVICE_OK;r->holds_buffers=1;r->owns_session=1;
 switch(queued.operation){
 case DMA_SERVICE_OPEN:phase=DMA_SERVICE_READY;break;
 case DMA_SERVICE_STATUS:break;
 case DMA_SERVICE_PERIOD_GET:
  if(phase==DMA_SERVICE_RESULT){r->result=DMA_SERVICE_BUSY;break;}
  r->period=(struct a733_i2s_period){.ticket=++ticket,.slot=phase==DMA_SERVICE_READY?primed:completed%4,
   .frames=2048,.kind=phase==DMA_SERVICE_READY?A733_I2S_TX_FILL:A733_I2S_TX_MEMORY_CONSUMED,.first_frame=(uint64_t)completed*2048};
  if(phase==DMA_SERVICE_RUNNING){check(memory[completed%4][0]==0x31+completed,20);++completed;}
  break;
 case DMA_SERVICE_PERIOD_PUT:
  check(queued.period_ticket==ticket&&ticket,21);++put_count;if(phase==DMA_SERVICE_READY)++primed;break;
 case DMA_SERVICE_SUBMIT:check(primed==4,22);phase=DMA_SERVICE_RUNNING;break;
 case DMA_SERVICE_CANCEL:phase=DMA_SERVICE_RESULT;outcome=DMA_SERVICE_CANCELED;break;
 default:check(0,23);
 }
 r->phase=phase;r->outcome=outcome;return 1;
}
static void begin(unsigned op,uint64_t sequence)
{position=wire_bytes=0;put32(input,op);put32(input+4,(uint32_t)sequence);put32(input+8,(uint32_t)(sequence>>32));available_bytes=12;}
static void finish_response(void)
{
 for(unsigned n=0;n<100&&(!response_bytes||response_sent<response_bytes);++n)exchange();
 check(response_bytes==128&&response_sent==128&&wire_bytes==128,30);
 check(get32(wire+4)==4&&get32(wire+16)==0,31);tud_vendor_tx_cb(0,128);check(!response_bytes,32);
}
static void upload(uint64_t sequence)
{
 begin(6,sequence);memset(input+12,0x30+sequence,4096);available_bytes=4108;
 finish_response();
}
static void complete(void){i2s_check_result=1;__asm__ volatile("brk #0");for(;;){}}
void i2s_check_run(unsigned selected)
{
 variant=selected;app.admitted=*(const struct i2s_session *)A733_STORAGE_SESSION_ADDRESS;app.session=app.admitted;
 check(i2s_session_valid(&app.session)&&app.session.direction==6,10);
 atomic_store(&app.audio_ready,1);atomic_store(&app.dma.state,A733_DMA_POOL_READY);
 for(unsigned i=0;i<4;++i)app.period_memory[i]=memory[i];
 if(variant==1){
  begin(6,1);memset(input+12,0x31,4096);available_bytes=4107;
  for(unsigned n=0;n<20&&position<available_bytes;++n)exchange();
  check(!atomic_load(&app.runtime.head)&&app.runtime.uploaded==4095&&!response_bytes,40);
  available_bytes=4108;finish_response();check(atomic_load(&app.runtime.head)==1,41);complete();
 }
 if(variant==2){
  for(unsigned n=1;n<=4;++n)upload(n);
  upload(5);check(get32(wire+108)==2&&atomic_load(&app.runtime.head)==4,42);
  for(unsigned n=0;n<4;++n)check(app.runtime.bytes[n][0]==0x31+n,43);complete();
 }
 if(variant==3){upload(1);expected_stop=1;upload(1);check(0,44);}
 if(variant==5){close_on_enter=1;upload(1);check(get32(wire+108)==2&&!atomic_load(&app.runtime.head)&&!atomic_load(&app.fatal),45);complete();}
 if(variant==8){
  begin(6,1);memset(input+12,0x31,4096);available_bytes=13;exchange();exchange();
  check(!atomic_load(&app.runtime.head)&&app.runtime.uploaded==1,46);tick+=24000000;expected_stop=1;exchange();check(0,47);
 }
 if(variant==4){
  for(unsigned n=0;n<30&&!app.pending;++n)check(i2s_client_step()==0,48);
  check(app.pending&&queued.operation==DMA_SERVICE_STATUS&&!put_count,49);upload(1);
  check(i2s_client_step()==0&&!app.pending&&!put_count&&!atomic_load(&app.runtime.committed),50);
  for(unsigned n=0;n<4&&!put_count;++n)check(i2s_client_step()==0,51);
  check(put_count==1&&atomic_load(&app.runtime.committed)==2048,52);complete();
 }
 unsigned uploaded=0,cancelled=0;
 for(unsigned n=0;n<1000;++n){
  if(uploaded<12&&i2s_runtime_credits()){upload(uploaded+1);check(get32(wire+108)==1,53);++uploaded;}
  if(variant==6&&app.pending&&phase==DMA_SERVICE_RUNNING){begin(2,0);finish_response();cancelled=1;}
  if(variant==7&&completed==4){phase=DMA_SERVICE_RESULT;outcome=DMA_SERVICE_FAILED;}
  if(!cancelled&&atomic_load(&app.runtime.consumed)>=8*2048){begin(2,0);finish_response();cancelled=1;}
  int r=i2s_client_step();if(!r)continue;
  check(r==(variant==7?-1:1)&&atomic_load(&app.runtime.drained)==1&&!atomic_load(&app.fatal),54);
  if(variant==7)check(atomic_load(&app.runtime.flags)==1,55);
  else check(atomic_load(&app.runtime.consumed)==(variant==6?2048u:8*2048u),56);
  begin(0,0);finish_response();check(get32(wire+104)==1,57);complete();
 }
 check(0,58);
}
