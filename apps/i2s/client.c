#include "application.h"
#include <string.h>
static int fault(void)
{atomic_store_explicit(&app.result,3,memory_order_release);return -1;}
static int request(enum dma_service_operation operation,uint64_t timeout,uint64_t period)
{
 if(!app.pending){
  struct dma_service_request r={.operation=operation,.job=0,.timeout=timeout,.period_ticket=period};
  if(!dma_service_send(&app.dma.service,&r,0,&app.pending))return 0;
 }
 if(!dma_service_collect(&app.dma.service,app.pending,&app.reply,0))return 0;
 app.pending=0;
 if(app.reply.result==DMA_SERVICE_BUSY)return 0;
 return app.reply.result==DMA_SERVICE_OK?1:-1;
}
int i2s_client_step(void)
{
 unsigned result=atomic_load_explicit(&app.result,memory_order_acquire);
 if(result==2)return 1;if(result==3)return -1;
 if(!i2s_live(NULL))return fault();
 if(!atomic_load_explicit(&app.audio_ready,memory_order_acquire)){
  if(atomic_load_explicit(&app.cancel,memory_order_acquire))return fault();
  int prepared=i2s_prepare_audio();if(prepared<0)return fault();return 0;
 }
 if(atomic_load_explicit(&app.dma.state,memory_order_acquire)==A733_DMA_POOL_FAILED)return fault();
 if(atomic_load_explicit(&app.dma.state,memory_order_acquire)!=A733_DMA_POOL_READY)return 0;
 /* Finish collecting each copied reply before selecting cancellation. A ticket
  * timeout never authorizes abandoning an in-flight request or period lease. */
 if(atomic_load_explicit(&app.cancel,memory_order_acquire)&&!app.pending&&app.stage<7){
  app.stopping=1;app.stage=7;
 }
 int r;
 switch(app.stage){
 case 0:
  atomic_store_explicit(&app.result,1,memory_order_release);
  r=request(DMA_SERVICE_OPEN,100000,0);if(r<0)return fault();if(!r)return 0;
  app.stage=1;return 0;
 case 1:
  r=request(DMA_SERVICE_STATUS,0,0);if(r<0)return fault();if(!r)return 0;
  if(app.reply.phase==DMA_SERVICE_OPENING)return 0;
  if(app.reply.phase!=DMA_SERVICE_READY)return fault();
  app.stage=app.session.direction?2:4;return 0;
 case 2:
  r=request(DMA_SERVICE_PERIOD_GET,0,0);if(r<0)return fault();if(!r)return 0;
  if(app.reply.period.kind!=A733_I2S_TX_FILL||app.reply.period.slot>=I2S_PERIODS||
     app.reply.period.frames!=I2S_PERIOD_BYTES/2||!app.reply.period.ticket)return fault();
  app.held_period=app.reply.period.ticket;
  memcpy(app.period_memory[app.reply.period.slot],
   i2s_recorded_pcm+app.reply.period.slot*I2S_PERIOD_BYTES,I2S_PERIOD_BYTES);
  app.stage=3;return 0;
 case 3:
  /* app.reply changes only after collection; preserve the held period ticket
   * while the copied PUT request is in flight. */
  r=request(DMA_SERVICE_PERIOD_PUT,0,app.held_period);if(r<0)return fault();if(!r)return 0;
  app.stage=++app.primed==I2S_PERIODS?4:2;return 0;
 case 4:
  r=request(DMA_SERVICE_SUBMIT,24000000,0);if(r<0)return fault();if(!r)return 0;
  app.stage=5;return 0;
 case 5:{
  r=request(DMA_SERVICE_PERIOD_GET,0,0);if(r<0)return fault();if(!r)return 0;
  struct a733_i2s_period p=app.reply.period;
  if(p.slot>=I2S_PERIODS||p.frames!=I2S_PERIOD_BYTES/2||!p.ticket||
     p.first_frame!=(uint64_t)app.periods*(I2S_PERIOD_BYTES/2)||
     p.kind!=(app.session.direction?A733_I2S_TX_MEMORY_CONSUMED:A733_I2S_CAPTURED))return fault();
  memcpy(app.output+app.periods*I2S_PERIOD_BYTES,app.period_memory[p.slot],I2S_PERIOD_BYTES);
  app.held_period=p.ticket;++app.periods;app.stage=app.periods==I2S_PERIODS?7:6;return 0;}
 case 6:
  r=request(DMA_SERVICE_PERIOD_PUT,0,app.held_period);if(r<0)return fault();if(!r)return 0;
  app.stage=5;return 0;
 case 7:
  r=request(DMA_SERVICE_CANCEL,0,0);if(r<0)return fault();if(!r)return 0;
  app.stage=8;return 0;
 case 8:
  r=request(DMA_SERVICE_STATUS,0,0);if(r<0)return fault();if(!r)return 0;
  if(app.reply.phase==DMA_SERVICE_DRAINING||app.reply.phase==DMA_SERVICE_RUNNING)return 0;
  if(app.reply.phase!=DMA_SERVICE_RESULT||app.reply.outcome!=DMA_SERVICE_CANCELED||!app.reply.holds_buffers)
   return fault();
  /* Every service reservation, including any final client-held period, stays
   * held. No REAP, CLOSE or allocator release occurs after this observation. */
  if(app.stopping||app.periods!=I2S_PERIODS||!i2s_live(NULL))return fault();
  atomic_store_explicit(&app.result,2,memory_order_release);return 1;
 default:return fault();
 }
}
void i2s_client_task(void *unused)
{(void)unused;for(;;){(void)i2s_client_step();vTaskDelay(1);}}
int i2s_output(unsigned offset,void *out,unsigned bytes)
{
 if(!out||bytes>4096||offset>I2S_PCM_BYTES||bytes>I2S_PCM_BYTES-offset||!i2s_live(NULL)||
    atomic_load_explicit(&app.cancel,memory_order_acquire)||atomic_load_explicit(&app.result,memory_order_acquire)!=2)return 0;
 memcpy(out,app.output+offset,bytes);
 return i2s_live(NULL)&&!atomic_load_explicit(&app.cancel,memory_order_acquire);
}
