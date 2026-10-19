#include "application.h"
#include <string.h>
static int fault(void)
{atomic_store_explicit(&app.result,3,memory_order_release);return -1;}
int i2s_request(enum dma_service_operation operation,uint64_t timeout,uint64_t period)
{
 if(!app.pending){
  struct dma_service_request r={.operation=operation,.job=app.job,.timeout=timeout,.period_ticket=period};
  if(!dma_service_send(&app.dma.service,&r,0,&app.pending))return 0;
 }
 if(!dma_service_collect(&app.dma.service,app.pending,&app.reply,0))return 0;
 app.pending=0;
 if(app.reply.result==DMA_SERVICE_BUSY)return 0;
 return app.reply.result==DMA_SERVICE_OK?1:-1;
}
unsigned i2s_output_bytes(void)
{
#ifdef SUNSTONE_I2S_TRANSCRIBE
 return app.session.capture_samples*2u;
#else
 return I2S_PCM_BYTES;
#endif
}
/* One copied request at a time; each reply is collected under its original job.
 * Success requires four actual DMA completions in EACH direction. */
static int duplex_step(void)
{
 int r;unsigned j=app.duplex_direction;
 if(atomic_load_explicit(&app.cancel,memory_order_acquire) && !app.pending &&
    app.duplex_stage>=2 && app.duplex_stage<8){app.stopping=1;app.duplex_stage=8;j=app.duplex_direction=0;}
 switch(app.duplex_stage){
 case 0:
  atomic_store_explicit(&app.result,1,memory_order_release);
  app.job=app.duplex_opened;
  r=i2s_request(DMA_SERVICE_OPEN,100000,0);if(r<0)return fault();if(!r)return 0;
  app.duplex_stage=1;return 0;
 case 1:
  r=i2s_request(DMA_SERVICE_STATUS,0,0);if(r<0)return fault();if(!r)return 0;
  if(app.reply.phase==DMA_SERVICE_OPENING)return 0;
  if(app.reply.phase!=DMA_SERVICE_READY)return fault();
  if(++app.duplex_opened<2){app.duplex_stage=0;return 0;}
  app.job=1;app.duplex_stage=2;return 0;
 case 2:{
  r=i2s_request(DMA_SERVICE_PERIOD_GET,0,0);if(r<0)return fault();if(!r)return 0;
  struct a733_i2s_period p=app.reply.period;
  if(p.kind!=A733_I2S_TX_FILL || p.slot!=app.primed || p.frames!=I2S_PERIOD_BYTES/2 || !p.ticket)return fault();
  app.playback_held=p.ticket;
  memcpy(app.playback_memory[p.slot],i2s_recorded_pcm+p.slot*I2S_PERIOD_BYTES,I2S_PERIOD_BYTES);
  app.duplex_stage=3;return 0;}
 case 3:
  r=i2s_request(DMA_SERVICE_PERIOD_PUT,0,app.playback_held);if(r<0)return fault();if(!r)return 0;
  app.duplex_stage=++app.primed==I2S_PERIODS?4:2;return 0;
 case 4:
  r=i2s_request(DMA_SERVICE_SUBMIT,24000000,0);if(r<0)return fault();if(!r)return 0;
  if(app.job==1){app.job=0;return 0;}
  app.duplex_direction=0;app.duplex_stage=5;return 0;
 case 5:{
  app.job=j;
  r=i2s_request(DMA_SERVICE_PERIOD_GET,0,0);if(r<0)return fault();if(!r)return 0;
  struct a733_i2s_period p=app.reply.period;
  unsigned n=j?app.playback_periods:app.periods;
  if(p.slot>=I2S_PERIODS || p.frames!=I2S_PERIOD_BYTES/2 || !p.ticket ||
     p.first_frame!=(uint64_t)n*(I2S_PERIOD_BYTES/2) ||
     p.kind!=(j?A733_I2S_TX_MEMORY_CONSUMED:A733_I2S_CAPTURED))return fault();
  if(j){
   if(app.playback_bytes>=I2S_PCM_BYTES || p.ticket==app.playback_held)return fault();
   app.playback_held=p.ticket;app.playback_bytes+=I2S_PERIOD_BYTES;++app.playback_periods;
  }else{
   if(app.output_bytes>=I2S_PCM_BYTES || p.ticket==app.held_period)return fault();
   memcpy(app.output+app.output_bytes,app.period_memory[p.slot],I2S_PERIOD_BYTES);
   app.held_period=p.ticket;app.output_bytes+=I2S_PERIOD_BYTES;++app.periods;
  }
  if(app.output_bytes==I2S_PCM_BYTES && app.playback_bytes==I2S_PCM_BYTES){
   app.duplex_stage=8;app.duplex_direction=0;return 0;
  }
  app.duplex_stage=6;return 0;}
 case 6:
  r=i2s_request(DMA_SERVICE_PERIOD_PUT,0,j?app.playback_held:app.held_period);if(r<0)return fault();if(!r)return 0;
  app.duplex_direction^=1;app.duplex_stage=5;return 0;
 case 8:
  app.job=j;
  r=i2s_request(DMA_SERVICE_CANCEL,0,0);if(r<0)return fault();if(!r)return 0;
  app.duplex_stage=9;return 0;
 case 9:
  r=i2s_request(DMA_SERVICE_STATUS,0,0);if(r<0)return fault();if(!r)return 0;
  if(app.reply.phase==DMA_SERVICE_DRAINING || app.reply.phase==DMA_SERVICE_RUNNING)return 0;
  if(app.reply.phase!=DMA_SERVICE_RESULT || app.reply.outcome!=DMA_SERVICE_CANCELED || !app.reply.holds_buffers)return fault();
  app.duplex_drained|=1u<<j;
  if(!j){app.duplex_direction=1;app.duplex_stage=8;return 0;}
  if(app.stopping || app.output_bytes!=I2S_PCM_BYTES || app.playback_bytes!=I2S_PCM_BYTES || !i2s_live(NULL))return fault();
  atomic_store_explicit(&app.result,2,memory_order_release);return 1;
 default:return fault();
 }
}
int i2s_client_step(void)
{
 unsigned result=atomic_load_explicit(&app.result,memory_order_acquire);
 if(result==2)return 1;if(result==3)return -1;
 if(!i2s_live(NULL))return fault();
 if(!atomic_load_explicit(&app.audio_ready,memory_order_acquire)){
  if(app.session.direction<3&&atomic_load_explicit(&app.cancel,memory_order_acquire))return fault();
  int prepared=i2s_prepare_audio();if(prepared<0)return fault();return 0;
 }
 if(atomic_load_explicit(&app.dma.state,memory_order_acquire)==A733_DMA_POOL_FAILED)return fault();
 if(atomic_load_explicit(&app.dma.state,memory_order_acquire)!=A733_DMA_POOL_READY)return 0;
 if(app.session.direction==I2S_RUNTIME_DIRECTION)return i2s_runtime_step();
 if(app.session.direction>=3)return i2s_repeated_step();
 if(app.session.direction==2)return duplex_step();
 /* Finish collecting each copied reply before selecting cancellation. A ticket
  * timeout never authorizes abandoning an in-flight request or period lease. */
 if(atomic_load_explicit(&app.cancel,memory_order_acquire)&&!app.pending&&app.stage<7){
  app.stopping=1;app.stage=7;
 }
 int r;
 switch(app.stage){
 case 0:
  atomic_store_explicit(&app.result,1,memory_order_release);
  r=i2s_request(DMA_SERVICE_OPEN,100000,0);if(r<0)return fault();if(!r)return 0;
  app.stage=1;return 0;
 case 1:
  r=i2s_request(DMA_SERVICE_STATUS,0,0);if(r<0)return fault();if(!r)return 0;
  if(app.reply.phase==DMA_SERVICE_OPENING)return 0;
  if(app.reply.phase!=DMA_SERVICE_READY)return fault();
  app.stage=app.session.direction?2:4;return 0;
 case 2:
  r=i2s_request(DMA_SERVICE_PERIOD_GET,0,0);if(r<0)return fault();if(!r)return 0;
  if(app.reply.period.kind!=A733_I2S_TX_FILL||app.reply.period.slot>=I2S_PERIODS||
     app.reply.period.frames!=I2S_PERIOD_BYTES/2||!app.reply.period.ticket)return fault();
  app.held_period=app.reply.period.ticket;
  memcpy(app.period_memory[app.reply.period.slot],
   i2s_recorded_pcm+app.reply.period.slot*I2S_PERIOD_BYTES,I2S_PERIOD_BYTES);
  app.stage=3;return 0;
 case 3:
  /* app.reply changes only after collection; preserve the held period ticket
   * while the copied PUT request is in flight. */
  r=i2s_request(DMA_SERVICE_PERIOD_PUT,0,app.held_period);if(r<0)return fault();if(!r)return 0;
  app.stage=++app.primed==I2S_PERIODS?4:2;return 0;
 case 4:
  r=i2s_request(DMA_SERVICE_SUBMIT,24000000,0);if(r<0)return fault();if(!r)return 0;
  app.stage=5;return 0;
 case 5:{
  r=i2s_request(DMA_SERVICE_PERIOD_GET,0,0);if(r<0)return fault();if(!r)return 0;
  struct a733_i2s_period p=app.reply.period;
  if(p.slot>=I2S_PERIODS||p.frames!=I2S_PERIOD_BYTES/2||!p.ticket||
     p.first_frame!=(uint64_t)app.periods*(I2S_PERIOD_BYTES/2)||
     p.kind!=(app.session.direction?A733_I2S_TX_MEMORY_CONSUMED:A733_I2S_CAPTURED))return fault();
  unsigned total=i2s_output_bytes();
  if(app.output_bytes>=total||p.ticket==app.held_period)return fault();
  unsigned bytes=total-app.output_bytes;if(bytes>I2S_PERIOD_BYTES)bytes=I2S_PERIOD_BYTES;
  memcpy(app.output+app.output_bytes,app.period_memory[p.slot],bytes);app.output_bytes+=bytes;
  app.held_period=p.ticket;++app.periods;app.stage=app.output_bytes==total?7:6;return 0;}
 case 6:
  r=i2s_request(DMA_SERVICE_PERIOD_PUT,0,app.held_period);if(r<0)return fault();if(!r)return 0;
  app.stage=5;return 0;
 case 7:
  r=i2s_request(DMA_SERVICE_CANCEL,0,0);if(r<0)return fault();if(!r)return 0;
  app.stage=8;return 0;
 case 8:
  r=i2s_request(DMA_SERVICE_STATUS,0,0);if(r<0)return fault();if(!r)return 0;
  if(app.reply.phase==DMA_SERVICE_DRAINING||app.reply.phase==DMA_SERVICE_RUNNING)return 0;
  if(app.reply.phase!=DMA_SERVICE_RESULT||app.reply.outcome!=DMA_SERVICE_CANCELED||!app.reply.holds_buffers)
   return fault();
  /* Every service reservation, including any final client-held period, stays
   * held. No REAP, CLOSE or allocator release occurs after this observation. */
  if(app.stopping||app.output_bytes!=i2s_output_bytes()||!i2s_live(NULL))return fault();
  atomic_store_explicit(&app.result,2,memory_order_release);return 1;
 default:return fault();
 }
}
void i2s_client_task(void *unused)
{(void)unused;for(;;){(void)i2s_client_step();
#ifdef SUNSTONE_I2S_TRANSCRIBE
 i2s_recognition_poll();
#endif
 vTaskDelay(1);}}
int i2s_output(unsigned offset,void *out,unsigned bytes)
{
 if(!out||bytes>4096||offset>i2s_output_bytes()||bytes>i2s_output_bytes()-offset||!i2s_live(NULL)||
    atomic_load_explicit(&app.cancel,memory_order_acquire)||atomic_load_explicit(&app.result,memory_order_acquire)!=2)return 0;
 memcpy(out,app.output+offset,bytes);
 return i2s_live(NULL)&&!atomic_load_explicit(&app.cancel,memory_order_acquire);
}

#ifdef SUNSTONE_I2S_TRANSCRIBE
int i2s_capture_valid(void)
{
 if(!i2s_live(NULL)||atomic_load_explicit(&app.cancel,memory_order_acquire)||
    atomic_load_explicit(&app.result,memory_order_acquire)==3||
    atomic_load_explicit(&app.dma.state,memory_order_acquire)==A733_DMA_POOL_FAILED)return -1;
 if(atomic_load_explicit(&app.result,memory_order_acquire)!=2)return 0;
 return !app.stopping&&app.stage==8&&app.output_bytes==i2s_output_bytes()&&
  app.reply.phase==DMA_SERVICE_RESULT&&app.reply.outcome==DMA_SERVICE_CANCELED&&app.reply.holds_buffers?1:-1;
}
unsigned i2s_capture_bytes(void){return i2s_output_bytes();}
int i2s_capture_copy(void *out,unsigned bytes)
{return out&&bytes==i2s_output_bytes()&&i2s_capture_valid()==1&&
 (memcpy(out,app.output,bytes),i2s_capture_valid()==1);}
int i2s_copy_target(const void *p,size_t bytes)
{
 uintptr_t a=(uintptr_t)p,b=(uintptr_t)&app;
 return p&&bytes<=UINTPTR_MAX-a&&(a+bytes<=b||b+sizeof(app)<=a);
}
#endif
