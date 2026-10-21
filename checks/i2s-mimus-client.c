#include "../apps/i2s/application.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct i2s_application app;
const uint8_t i2s_recorded_pcm[I2S_PCM_BYTES]={1};
const uint8_t i2s_recorded_pcm_digest[32]={1};
static uint8_t memory[I2S_PERIODS][I2S_PERIOD_BYTES];
static struct dma_service_request queued;
static unsigned sent,consumed,put_count,cancelled,draining,mode;
static int live=1;
int i2s_live(void *unused){(void)unused;return live;}
int i2s_prepare_audio(void){atomic_store(&app.audio_ready,1);return 1;}
void i2s_recognition_poll(void){}
/* Finite capture (direction 0) never reaches the repeated or runtime paths. */
int i2s_repeated_step(void){abort();}
int i2s_runtime_step(void){abort();}
void vTaskDelay(TickType_t ticks){(void)ticks;}
int dma_service_send(struct dma_service *s,const struct dma_service_request *r,TickType_t wait,uint64_t *ticket)
{(void)s;assert(!wait);queued=*r;*ticket=++sent;return 1;}
int dma_service_collect(struct dma_service *s,uint64_t ticket,struct dma_service_reply *out,TickType_t wait)
{
 (void)s;assert(ticket==sent&&!wait);memset(out,0,sizeof(*out));out->result=DMA_SERVICE_OK;out->holds_buffers=1;
 switch(queued.operation){
 case DMA_SERVICE_OPEN:out->phase=DMA_SERVICE_READY;break;
 case DMA_SERVICE_STATUS:
  if(cancelled){out->phase=draining++?DMA_SERVICE_RESULT:DMA_SERVICE_DRAINING;
   out->outcome=mode==3?DMA_SERVICE_FAILED:DMA_SERVICE_CANCELED;}
  else out->phase=DMA_SERVICE_READY;break;
 case DMA_SERVICE_SUBMIT:out->phase=DMA_SERVICE_RUNNING;break;
 case DMA_SERVICE_PERIOD_GET:
  out->period=(struct a733_i2s_period){.ticket=100+sent,.slot=consumed%4,.frames=2048,
   .first_frame=(uint64_t)consumed*2048,.kind=A733_I2S_CAPTURED};
  if(consumed==6&&mode==1)out->period.first_frame+=2048;
  if(consumed==6&&mode==2)out->period.ticket=app.held_period;
  memset(memory[consumed%4],(int)(1+consumed),I2S_PERIOD_BYTES);++consumed;break;
 case DMA_SERVICE_PERIOD_PUT:assert(queued.period_ticket);++put_count;break;
 case DMA_SERVICE_CANCEL:cancelled=1;out->phase=DMA_SERVICE_DRAINING;break;
 default:assert(0);
 }
 return 1;
}
int main(int argc,char **argv)
{
 assert(argc==2);mode=(unsigned)atoi(argv[1]);app.session.capture_samples=80000;
 atomic_store(&app.dma.state,A733_DMA_POOL_READY);
 for(unsigned i=0;i<4;++i)app.period_memory[i]=memory[i];
 uint8_t copied[4096];assert(!i2s_output(0,copied,sizeof(copied))&&i2s_capture_valid()==0);
 int r=0;for(unsigned step=0;step<600&&!r;++step){
  if(mode==4&&consumed==8)atomic_store(&app.cancel,1);
  r=i2s_client_step();if(!r)assert(!i2s_output(0,copied,sizeof(copied)));
 }
 if(mode){assert(r==-1&&i2s_capture_valid()==-1&&!i2s_output(0,copied,1));}
 else{
  assert(r==1&&consumed==40&&put_count==39&&cancelled&&draining>=2);
  assert(app.output_bytes==160000&&app.periods==40&&i2s_capture_valid()==1);
  for(unsigned offset=0;offset<160000;++offset)assert(app.output[offset]==1+offset/4096);
  assert(i2s_output(159744,copied,256)&&copied[255]==40);
  assert(!i2s_output(159999,copied,2));
  live=0;assert(i2s_capture_valid()==-1&&!i2s_output(0,copied,1));
 }
 puts("PASS finite I2S copy ordering, ring reuse, planned stop and retained refusal");return 0;
}
