/* Actual native client and USB framing; endpoint transport/service effects modeled. */
#include "../apps/i2s/usb0.c"
struct i2s_application app;
volatile unsigned i2s_check_result;
static uint8_t input[4108],wire[160],memory[4][4096];
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
  if(variant==5&&completed==2){phase=DMA_SERVICE_RESULT;outcome=DMA_SERVICE_FAILED;}
  if(phase==DMA_SERVICE_RESULT){r->result=DMA_SERVICE_BUSY;break;}
  r->period=(struct a733_i2s_period){.ticket=++ticket,.slot=phase==DMA_SERVICE_READY?primed:completed%4,
   .frames=phase==DMA_SERVICE_RUNNING&&completed==2?1:2048,.kind=phase==DMA_SERVICE_READY?A733_I2S_TX_FILL:A733_I2S_TX_MEMORY_CONSUMED,.first_frame=(uint64_t)completed*2048};
  if(phase==DMA_SERVICE_RUNNING){check(memory[completed%4][0]==0x31+completed,20);++completed;if(completed==3){phase=DMA_SERVICE_RESULT;outcome=DMA_SERVICE_COMPLETE;}}
  break;
 case DMA_SERVICE_PERIOD_PUT:
  check(queued.period_ticket==ticket&&ticket,21);++put_count;if(phase==DMA_SERVICE_READY)++primed;break;
 case DMA_SERVICE_PERIOD_END:check(queued.period_ticket==ticket&&queued.timeout==1,24);++put_count;++primed;break;
 case DMA_SERVICE_SUBMIT:check(primed==3,22);phase=DMA_SERVICE_RUNNING;break;
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
 check(response_bytes==160&&response_sent==160&&wire_bytes==160,30);
 check(get32(wire+4)==5&&get32(wire+16)==0,31);tud_vendor_tx_cb(0,160);check(!response_bytes,32);
}
static void upload(uint64_t sequence)
{
 begin(6,sequence);unsigned bytes=i2s_runtime_upload_bytes(sequence);memset(input+12,0x30+sequence,bytes);available_bytes=12+bytes;
 finish_response();
}
static void complete(void){i2s_check_result=1;__asm__ volatile("brk #0");for(;;){}}
void i2s_check_run(unsigned selected)
{
 variant=selected;app.admitted=*(const struct i2s_session *)A733_STORAGE_SESSION_ADDRESS;app.session=app.admitted;
 check(i2s_session_valid(&app.session)&&app.session.direction==6,10);
 atomic_store(&app.audio_ready,1);atomic_store(&app.dma.state,A733_DMA_POOL_READY);
 for(unsigned i=0;i<4;++i)app.period_memory[i]=memory[i];
 begin(7,variant==1?0:4097);finish_response();
 if(variant==1){check(get32(wire+108)==3&&!atomic_load(&app.runtime.file_mode),50);complete();}
 check(get32(wire+108)==1,51);
 if(variant==2){begin(8,4097);finish_response();check(get32(wire+108)==3&&!atomic_load(&app.runtime.eof),52);complete();}
 upload(1);upload(2);
 begin(6,3);input[12]=0x33;available_bytes=13;exchange();exchange();
 check(!response_bytes&&atomic_load(&app.runtime.head)==2&&app.runtime.uploaded==1,53);
 if(variant==3){check(!i2s_runtime_end(4097),54);complete();}
 input[13]=0x33;available_bytes=14;finish_response();check(atomic_load(&app.runtime.head)==3,55);
 begin(8,4097);finish_response();check(get32(wire+108)==1&&atomic_load(&app.runtime.eof),56);
 if(variant==4){check(!i2s_runtime_end(4097)&&!i2s_runtime_begin(1)&&i2s_runtime_upload(4,0,input,2)<=0,57);complete();}
 for(unsigned n=0;n<300;++n){int r=i2s_client_step();if(r){if(variant==5){check(r==-1&&atomic_load(&app.runtime.flags)&&atomic_load(&app.runtime.drained)&&completed==2,63);complete();}
   check(r==1&&completed==3&&put_count==6,58);
   begin(0,0);finish_response();check(get32(wire+132)==1&&get32(wire+136)==1&&!get32(wire+140)&&!get32(wire+144),59);
   check(atomic_load(&app.runtime.committed)==4097&&atomic_load(&app.runtime.consumed)==4097,60);
   begin(2,0);finish_response();for(unsigned k=0;k<30&&!atomic_load(&app.runtime.stopped);++k)i2s_client_step();
   check(atomic_load(&app.runtime.stopped)&&outcome==DMA_SERVICE_CANCELED,61);complete();}}
 check(0,62);
}
