#include "../apps/i2s/application.h"
#include <string.h>
struct i2s_application app;
const uint8_t i2s_recorded_pcm[I2S_PCM_BYTES]={0x31,0x42};
const uint8_t i2s_recorded_pcm_digest[32]={1};
volatile unsigned i2s_check_result;
static uint8_t memory[I2S_PERIODS][4096],playback[I2S_PERIODS][4096];
static unsigned counts[2],cancelled_jobs,delayed_get;
static struct dma_service_request queued;
static unsigned sent,consumed,fill,mode,put_count,cancelled,drain_observations;
static uint64_t tick=240;
int i2s_live(void *v){(void)v;return tick<app.deadline*24;}
int i2s_prepare_audio(void){atomic_store(&app.audio_ready,1);return 1;}
void vTaskDelay(TickType_t ticks){(void)ticks;}
int dma_service_send(struct dma_service *s,const struct dma_service_request *r,TickType_t wait,uint64_t *ticket)
{(void)s;(void)wait;queued=*r;*ticket=++sent;return 1;}
int dma_service_collect(struct dma_service *s,uint64_t ticket,struct dma_service_reply *out,TickType_t wait)
{
 (void)s;(void)ticket;(void)wait;memset(out,0,sizeof(*out));out->result=DMA_SERVICE_OK;out->holds_buffers=1;
 switch(queued.operation){
 case DMA_SERVICE_OPEN:out->phase=DMA_SERVICE_READY;break;
 case DMA_SERVICE_STATUS:
  if(mode>=5){
   if(cancelled_jobs&(1u<<queued.job)){out->phase=DMA_SERVICE_RESULT;out->outcome=DMA_SERVICE_CANCELED;}
   else out->phase=DMA_SERVICE_READY;break;
  }
  if(cancelled){out->phase=drain_observations++?DMA_SERVICE_RESULT:DMA_SERVICE_DRAINING;
   out->outcome=mode==4?DMA_SERVICE_FAILED:DMA_SERVICE_CANCELED;}
  else out->phase=DMA_SERVICE_READY;break;
 case DMA_SERVICE_PERIOD_GET:
  if(mode>=5){
   unsigned j=queued.job;
   if(mode==6 && j && app.primed==I2S_PERIODS){
    out->result=DMA_SERVICE_BUSY;if(++delayed_get>3)tick=app.deadline*24;break;
   }
   unsigned priming=j && app.primed<I2S_PERIODS,n=priming?fill++:counts[j];
   out->period=(struct a733_i2s_period){.ticket=100+sent,.slot=n%4,.frames=2048,
    .first_frame=(uint64_t)n*2048,.kind=priming?A733_I2S_TX_FILL:
    j?A733_I2S_TX_MEMORY_CONSUMED:A733_I2S_CAPTURED};
   if(!priming){
    if(!j)memset(memory[n%4],0x41+n,4096);
    else if(memcmp(playback[n%4],i2s_recorded_pcm+(n%4)*4096,4096)){
     i2s_check_result=29;__asm__ volatile("brk #0");for(;;){}
    }
    ++counts[j];
   }
   break;
  }
  out->period=(struct a733_i2s_period){.ticket=100+sent,.slot=app.primed<I2S_PERIODS&&app.session.direction?fill++:consumed%4,
   .frames=2048,.first_frame=(uint64_t)consumed*2048,
   .kind=app.primed<I2S_PERIODS&&app.session.direction?A733_I2S_TX_FILL:
     app.session.direction?A733_I2S_TX_MEMORY_CONSUMED:A733_I2S_CAPTURED};
  if(out->period.kind!=A733_I2S_TX_FILL){
   if(!app.session.direction)memset(memory[consumed%4],(int)(0x41+consumed),4096);
   ++consumed;
  }break;
 case DMA_SERVICE_PERIOD_PUT:
  if(!queued.period_ticket){out->result=DMA_SERVICE_INVALID;break;}++put_count;break;
 case DMA_SERVICE_SUBMIT:out->phase=DMA_SERVICE_RUNNING;break;
 case DMA_SERVICE_CANCEL:cancelled_jobs|=1u<<queued.job;cancelled=1;out->phase=DMA_SERVICE_DRAINING;break;
 default:out->result=DMA_SERVICE_INVALID;
 }
 return 1;
}
static void check(int ok,unsigned code){if(!ok){i2s_check_result=code;__asm__ volatile("brk #0");for(;;){}}}
void i2s_check_run(unsigned selected)
{
 mode=selected;app.deadline=100000;app.session.direction=mode>=5?2:mode==1;
 atomic_store(&app.dma.state,A733_DMA_POOL_READY);
 for(unsigned i=0;i<4;++i){app.period_memory[i]=memory[i];app.playback_memory[i]=playback[i];}
 if(mode==2)atomic_store(&app.cancel,1);
 if(mode==3)tick=app.deadline*24;
 uint8_t copy[4096];check(!i2s_output(0,copy,sizeof(copy)),10);
 int r=0;for(unsigned n=0;n<100&&!r;++n){
  r=i2s_client_step();if(mode==7 && app.duplex_stage==5)atomic_store(&app.cancel,1);if(!r)check(!i2s_output(0,copy,sizeof(copy)),11);
 }
 if(mode==5){
  check(r==1 && counts[0]==4 && counts[1]==4 && cancelled_jobs==3,25);
  check(app.output_bytes==I2S_PCM_BYTES && app.playback_bytes==I2S_PCM_BYTES,26);
  check(i2s_output(0,copy,sizeof(copy)) && copy[0]==0x41 && app.output[16383]==0x44,27);
 }else if(mode>=2){check(r==-1&&atomic_load(&app.result)==3,12);check(!i2s_output(0,copy,1),13);}
 else{
  check(r==1&&consumed==4&&cancelled&&drain_observations>=2,14);
  check(put_count==(app.session.direction?7:3),15);
  check(i2s_output(0,copy,sizeof(copy)),16);
  if(app.session.direction)check(copy[0]==0x31&&copy[1]==0x42,17);
  else check(copy[0]==0x41&&app.output[16383]==0x44,18);
  check(!i2s_output(I2S_PCM_BYTES-1,copy,2),19);
 }
 i2s_check_result=1;__asm__ volatile("brk #0");for(;;){}
}
