#include "application.h"
#include <string.h>
_Static_assert(ATOMIC_LLONG_LOCK_FREE==2,"Repeated I2S requires lock-free 64-bit observations");
enum { OPEN,WAIT_OPEN,PRIME,PUT_PRIME,START,GET,PUT,CHECK,STOP_STATUS,STOP_CANCEL };
static int failed(void)
{atomic_store_explicit(&app.result,3,memory_order_release);i2s_fail(NULL);return -1;}
static unsigned mode(void){return app.session.direction%3;}
static unsigned jobs(void){return mode()==2?2:1;}
static int transmit(unsigned job){return mode()==1||job==1;}
static void stopping(unsigned flag)
{
 atomic_fetch_or_explicit(&app.repeated.flags,flag,memory_order_release);
 app.repeated.direction=0;app.repeated.stage=STOP_STATUS;
}
int i2s_repeated_copy(unsigned offset,unsigned bytes,void *out,struct i2s_repeated_period *period)
{
 if(!i2s_live(NULL)||!out||!period||bytes>4096||offset>4096||bytes>4096-offset)return -1;
 uint64_t tail=atomic_load_explicit(&app.repeated.tail,memory_order_relaxed);
 uint64_t head=atomic_load_explicit(&app.repeated.head,memory_order_acquire);
 if(head<tail||head-tail>I2S_REPEATED_SLOTS)return -1;
 if(head==tail)return 0;
 const struct i2s_repeated_slot *slot=&app.repeated.slots[tail%I2S_REPEATED_SLOTS];
 if(tail==UINT64_MAX||slot->period.ticket!=tail+1)return -1;
 *period=slot->period;memcpy(out,slot->bytes+offset,bytes);
 return i2s_live(NULL)?1:-1;
}
int i2s_repeated_delivered(uint64_t ticket,unsigned offset,unsigned bytes)
{
 struct i2s_repeated *s=&app.repeated;
 uint64_t tail=atomic_load_explicit(&s->tail,memory_order_relaxed);
 uint64_t head=atomic_load_explicit(&s->head,memory_order_acquire);
 if(!i2s_live(NULL)||tail==UINT64_MAX||!ticket||ticket!=tail+1||head<=tail||
  bytes>4096||offset>4096||bytes>4096-offset)return 0;
 if(!s->delivered_ticket){if(offset)return 0;s->delivered_ticket=ticket;}
 if(s->delivered_ticket!=ticket||offset>s->delivered_bytes)return 0;
 if(offset+bytes>s->delivered_bytes)s->delivered_bytes=offset+bytes;
 return 1;
}
int i2s_repeated_ack(uint64_t ticket)
{
 if(!i2s_live(NULL)||!ticket||app.repeated.delivered_ticket!=ticket||app.repeated.delivered_bytes!=4096)return 0;
 uint64_t tail=atomic_load_explicit(&app.repeated.tail,memory_order_relaxed);
 uint64_t head=atomic_load_explicit(&app.repeated.head,memory_order_acquire);
 if(tail==UINT64_MAX||head<=tail||head-tail>I2S_REPEATED_SLOTS||ticket!=tail+1)return 0;
 /* USB calls only after complete copied-period delivery and exact ACK. */
 atomic_store_explicit(&app.repeated.tail,tail+1,memory_order_release);
 app.repeated.delivered_ticket=0;app.repeated.delivered_bytes=0;return 1;
}
int i2s_repeated_step(void)
{
 struct i2s_repeated *s=&app.repeated;unsigned job=s->direction;int r;
 if(atomic_load_explicit(&app.cancel,memory_order_acquire)&&!app.pending&&s->stage>=PRIME&&s->stage<STOP_STATUS){
  stopping(0);job=0;
 }
 switch(s->stage){
 case OPEN:
  atomic_store_explicit(&app.result,1,memory_order_release);app.job=s->opened;
  r=i2s_request(DMA_SERVICE_OPEN,100000,0);if(r<0)return failed();if(!r)return 0;
  s->stage=WAIT_OPEN;return 0;
 case WAIT_OPEN:
  r=i2s_request(DMA_SERVICE_STATUS,0,0);if(r<0)return failed();if(!r)return 0;
  if(app.reply.phase==DMA_SERVICE_OPENING)return 0;
  if(app.reply.phase!=DMA_SERVICE_READY)return failed();
  if(++s->opened<jobs()){s->stage=OPEN;return 0;}
  app.job=jobs()-1;s->stage=mode()?PRIME:START;return 0;
 case PRIME:{
  r=i2s_request(DMA_SERVICE_PERIOD_GET,0,0);if(r<0)return failed();if(!r)return 0;
  struct a733_i2s_period p=app.reply.period;
  if(p.kind!=A733_I2S_TX_FILL||p.slot!=s->primed||p.frames!=I2S_PERIOD_BYTES/2||!p.ticket)return failed();
  unsigned txjob=jobs()-1;s->held[txjob]=p.ticket;
  void *memory=mode()==2?app.playback_memory[p.slot]:app.period_memory[p.slot];
  memcpy(memory,i2s_recorded_pcm+p.slot*I2S_PERIOD_BYTES,I2S_PERIOD_BYTES);
  s->stage=PUT_PRIME;return 0;}
 case PUT_PRIME:
  r=i2s_request(DMA_SERVICE_PERIOD_PUT,0,s->held[jobs()-1]);if(r<0)return failed();if(!r)return 0;
  s->stage=++s->primed==I2S_PERIODS?START:PRIME;return 0;
 case START:
  r=i2s_request(DMA_SERVICE_SUBMIT,24000000,0);if(r<0)return failed();if(!r)return 0;
  if(mode()==2&&app.job==1){app.job=0;return 0;}
  s->direction=0;s->stage=GET;return 0;
 case GET:{
  app.job=job;r=i2s_request(DMA_SERVICE_PERIOD_GET,0,0);if(r<0)return failed();
  if(!r){if(!app.pending&&app.reply.result==DMA_SERVICE_BUSY)s->stage=CHECK;return 0;}
  struct a733_i2s_period p=app.reply.period;unsigned tx=transmit(job);
  _Atomic uint64_t *frames=tx?&s->tx_frames:&s->rx_frames;
  uint64_t previous=atomic_load_explicit(frames,memory_order_relaxed);
  unsigned cyclic=app.stream[job].stream.cyclic;
  enum a733_i2s_period_kind expected=tx?(cyclic?A733_I2S_TX_REPEAT_PROGRESS:A733_I2S_TX_MEMORY_CONSUMED):
      (cyclic?A733_I2S_CAPTURED_COPY:A733_I2S_CAPTURED);
  if(p.slot>=I2S_PERIODS || !p.frames || p.frames%(I2S_PERIOD_BYTES/2) ||
   (!tx && p.frames!=I2S_PERIOD_BYTES/2) || (!cyclic && p.frames!=I2S_PERIOD_BYTES/2) ||
   !p.ticket || p.ticket<=s->held[job] || p.first_frame!=previous ||
   previous>UINT64_MAX-p.frames || p.kind!=expected)return failed();
  s->held[job]=p.ticket;
  atomic_store_explicit(frames,previous+p.frames,memory_order_release);
  if(!tx){
   uint64_t head=atomic_load_explicit(&s->head,memory_order_relaxed);
   uint64_t tail=atomic_load_explicit(&s->tail,memory_order_acquire);
   if(head<tail||head-tail>I2S_REPEATED_SLOTS||head==UINT64_MAX)return failed();
   if(head-tail==I2S_REPEATED_SLOTS){stopping(I2S_REPEATED_OVERRUN);return 0;}
   struct i2s_repeated_slot *slot=&s->slots[head%I2S_REPEATED_SLOTS];
   if(cyclic) {
    if(!dma_service_copy_period(&app.dma.service,job,p.ticket,slot->bytes,I2S_PERIOD_BYTES))return failed();
   }else memcpy(slot->bytes,app.period_memory[p.slot],I2S_PERIOD_BYTES);
   slot->period=(struct i2s_repeated_period){head+1,p.first_frame,p.frames,p.counter};
   atomic_store_explicit(&s->head,head+1,memory_order_release);
  }
  if(tx && cyclic){s->direction=(job+1)%jobs();s->stage=GET;}
  else s->stage=PUT;return 0;}
 case PUT:
  r=i2s_request(DMA_SERVICE_PERIOD_PUT,0,s->held[job]);if(r<0)return failed();if(!r)return 0;
  s->direction=(job+1)%jobs();s->stage=GET;return 0;
 case CHECK:
  r=i2s_request(DMA_SERVICE_STATUS,0,0);if(r<0)return failed();if(!r)return 0;
  if(app.reply.phase==DMA_SERVICE_RESULT||app.reply.phase==DMA_SERVICE_DRAINING){stopping(I2S_REPEATED_DEVICE_FAILED);return 0;}
  if(app.reply.phase!=DMA_SERVICE_RUNNING&&app.reply.phase!=DMA_SERVICE_READY)return failed();
  s->stage=GET;return 0;
 case STOP_STATUS:
  app.job=job;r=i2s_request(DMA_SERVICE_STATUS,0,0);if(r<0)return failed();if(!r)return 0;
  if(app.stream[job].stream.cyclic) {
   _Atomic uint64_t *frames=transmit(job)?&s->tx_frames:&s->rx_frames;
   uint64_t previous=atomic_load_explicit(frames,memory_order_relaxed);
   if(app.reply.stream_frames<previous || app.reply.stream_frames%(I2S_PERIOD_BYTES/2))return failed();
   atomic_store_explicit(frames,app.reply.stream_frames,memory_order_release);
  }
  if(app.reply.phase==DMA_SERVICE_RUNNING||app.reply.phase==DMA_SERVICE_READY){s->stage=STOP_CANCEL;return 0;}
  if(app.reply.phase==DMA_SERVICE_DRAINING)return 0;
  if(app.reply.phase!=DMA_SERVICE_RESULT||!app.reply.holds_buffers||
   (app.reply.outcome!=DMA_SERVICE_CANCELED&&app.reply.outcome!=DMA_SERVICE_FAILED))return failed();
  if(app.reply.outcome==DMA_SERVICE_FAILED)atomic_fetch_or_explicit(&s->flags,I2S_REPEATED_DEVICE_FAILED,memory_order_release);
  atomic_fetch_or_explicit(&s->drained,1u<<job,memory_order_release);
  if(++s->direction<jobs())return 0;
  atomic_store_explicit(&app.result,atomic_load_explicit(&s->flags,memory_order_acquire)?3:2,memory_order_release);
  return atomic_load_explicit(&s->flags,memory_order_acquire)?-1:1;
 case STOP_CANCEL:
  r=i2s_request(DMA_SERVICE_CANCEL,0,0);if(r<0)return failed();
  if(!r&&app.pending)return 0;
  s->stage=STOP_STATUS;return 0;
 default:return failed();
 }
}
