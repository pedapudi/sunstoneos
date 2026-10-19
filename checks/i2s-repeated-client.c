#include "../apps/i2s/application.h"
#include <string.h>
struct i2s_application app;
volatile unsigned i2s_check_result;
static uint8_t rx[I2S_PERIODS][4096],tx[I2S_PERIODS][4096];
static struct dma_service_request queued;
static unsigned selected,phase[2],outcome[2],drains[2],primed[2],completed[2],sent,cancel_race,delayed;
static uint64_t generations[2];
static void check(int ok,unsigned code){if(!ok){i2s_check_result=code;__asm__ volatile("brk #0");for(;;){}}}
int i2s_live(void *p){(void)p;return !atomic_load(&app.fatal);}
void i2s_fail(void *p){(void)p;atomic_store(&app.fatal,1);}
int i2s_prepare_audio(void){atomic_store(&app.audio_ready,1);return 1;}
void vTaskDelay(TickType_t ticks){(void)ticks;}
int dma_service_send(struct dma_service *s,const struct dma_service_request *r,TickType_t wait,uint64_t *ticket)
{(void)s;(void)wait;queued=*r;*ticket=++sent;return 1;}
int dma_service_collect(struct dma_service *s,uint64_t ticket,struct dma_service_reply *out,TickType_t wait)
{
 (void)s;(void)ticket;(void)wait;
 if(selected==7&&queued.operation==DMA_SERVICE_PERIOD_GET&&phase[queued.job]==DMA_SERVICE_RUNNING&&!delayed){
  delayed=1;atomic_store(&app.cancel,1);return 0;
 }
 unsigned j=queued.job,mode=app.session.direction%3,transmit=mode==1||j==1;
 memset(out,0,sizeof(*out));out->result=DMA_SERVICE_OK;out->holds_buffers=1;
 switch(queued.operation){
 case DMA_SERVICE_OPEN:phase[j]=DMA_SERVICE_READY;break;
 case DMA_SERVICE_STATUS:
  if(phase[j]==DMA_SERVICE_DRAINING&&++drains[j]>=2)phase[j]=DMA_SERVICE_RESULT;
  break;
 case DMA_SERVICE_SUBMIT:phase[j]=DMA_SERVICE_RUNNING;break;
 case DMA_SERVICE_PERIOD_GET:
  if(phase[j]==DMA_SERVICE_READY&&transmit){
   out->period=(struct a733_i2s_period){.ticket=++generations[j],.slot=primed[j]++,
    .frames=2048,.kind=A733_I2S_TX_FILL};break;
  }
  if(selected==4&&transmit&&completed[j]==3&&phase[j]==DMA_SERVICE_RUNNING){
   phase[j]=DMA_SERVICE_DRAINING;outcome[j]=DMA_SERVICE_FAILED;
  }
  if(phase[j]!=DMA_SERVICE_RUNNING){out->result=DMA_SERVICE_BUSY;break;}
  {unsigned n=completed[j]++,slot=n%4;
   out->period=(struct a733_i2s_period){.ticket=++generations[j],.slot=slot,.frames=2048,
    .first_frame=(uint64_t)n*2048,.counter=(n+1)*2048,
    .kind=transmit?A733_I2S_TX_MEMORY_CONSUMED:A733_I2S_CAPTURED};
   if(transmit){const void *p=mode==2?tx[slot]:rx[slot];check(!memcmp(p,i2s_recorded_pcm+slot*4096,4096),20);}
   else memset(rx[slot],0x40+n,4096);
  }break;
 case DMA_SERVICE_PERIOD_PUT:check(queued.period_ticket==generations[j],21);break;
 case DMA_SERVICE_CANCEL:
  if(selected==5&&!cancel_race++){
   phase[j]=DMA_SERVICE_DRAINING;outcome[j]=DMA_SERVICE_CANCELED;out->result=DMA_SERVICE_BUSY;break;
  }
  check(phase[j]==DMA_SERVICE_RUNNING||phase[j]==DMA_SERVICE_READY,22);
  phase[j]=DMA_SERVICE_DRAINING;outcome[j]=DMA_SERVICE_CANCELED;break;
 default:check(0,23);
 }
 out->phase=phase[j];out->outcome=outcome[j];return 1;
}
static void consume(void)
{
 struct i2s_repeated_period p={0};uint8_t copy[4096];int got=i2s_repeated_copy(0,4096,copy,&p);
 check(got>=0,30);if(!got)return;
 check(copy[0]==(uint8_t)(0x40+p.first_frame/2048)&&copy[4095]==copy[0],31);
 check(!i2s_repeated_ack(p.ticket),32);
 check(i2s_repeated_delivered(p.ticket,0,2048)&&!i2s_repeated_ack(p.ticket),33);
 check(i2s_repeated_delivered(p.ticket,2048,2048),34);
 check(!i2s_repeated_ack(p.ticket+1)&&i2s_repeated_ack(p.ticket)&&!i2s_repeated_ack(p.ticket),35);
}
void i2s_check_run(unsigned variant)
{
 selected=variant;app.admitted=*(const struct i2s_session *)A733_STORAGE_SESSION_ADDRESS;
 app.session=app.admitted;app.session.direction=variant<3?3+variant:5;
 app.session.dma.service_channels=app.session.direction%3==2?3:1;
 check(i2s_session_valid(&app.session),10);
 atomic_store(&app.dma.state,A733_DMA_POOL_READY);
 for(unsigned i=0;i<4;++i){app.period_memory[i]=rx[i];app.playback_memory[i]=tx[i];}
 int result=0;unsigned cancelled=0;uint8_t preserved[4096];
 for(unsigned n=0;n<2000&&!result;++n){
  result=i2s_client_step();
  uint64_t head=atomic_load(&app.repeated.head),tail=atomic_load(&app.repeated.tail);
  if(variant!=3&&variant!=4&&!(variant==6&&tail>=8))consume();
  if(variant==3&&head==4&&tail==0&&!cancelled){memcpy(preserved,app.repeated.slots[0].bytes,4096);cancelled=1;}
  uint64_t progress=app.session.direction%3==1?atomic_load(&app.repeated.tx_frames):atomic_load(&app.repeated.rx_frames);
  if(variant!=3&&variant!=4&&progress>=(variant==6?10u:12u)*2048){atomic_store(&app.cancel,1);cancelled=1;}
 }
 if(variant==3||variant==4){
  check(result==-1&&atomic_load(&app.result)==3&&!atomic_load(&app.fatal),40);
  check(atomic_load(&app.repeated.drained)==3,41);
  check(atomic_load(&app.repeated.flags)==(variant==3?I2S_REPEATED_OVERRUN:I2S_REPEATED_DEVICE_FAILED),42);
  if(variant==3)check(atomic_load(&app.repeated.head)==4&&!memcmp(preserved,app.repeated.slots[0].bytes,4096),43);
 }else{
  check(result==1&&atomic_load(&app.result)==2&&!atomic_load(&app.fatal),44);
  check(atomic_load(&app.repeated.drained)==(app.session.direction%3==2?3u:1u),45);
  if(variant==6){check(atomic_load(&app.repeated.head)>atomic_load(&app.repeated.tail),46);while(atomic_load(&app.repeated.head)>atomic_load(&app.repeated.tail))consume();}
  if(variant==7)check(delayed,47);
 }
 i2s_check_result=1;__asm__ volatile("brk #0");for(;;){}
}
