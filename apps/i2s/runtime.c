#include "application.h"
#include <string.h>
_Static_assert(ATOMIC_LLONG_LOCK_FREE==2,"Runtime PCM requires lock-free sequence observations");
enum { OPEN,WAIT_OPEN,PRIME_GET,FILL,WAIT_INPUT,PUT,START,GET,CHECK,STOP_STATUS,STOP_CANCEL };
void i2s_runtime_upload_discard(void){app.runtime.uploaded=0;}
unsigned i2s_runtime_credits(void)
{
 struct i2s_runtime *s=&app.runtime;
 uint64_t h=atomic_load_explicit(&s->head,memory_order_acquire),t=atomic_load_explicit(&s->tail,memory_order_acquire);
 if(atomic_load_explicit(&s->closed,memory_order_acquire)||atomic_load_explicit(&app.result,memory_order_acquire)>=2||h<t||h-t>I2S_RUNTIME_SLOTS)return 0;
 return I2S_RUNTIME_SLOTS-(unsigned)(h-t);
}
/* USB is the only producer. It supplies contiguous fragments of the exact next
 * sequence. A partially written slot is never visible to the audio consumer. */
int i2s_runtime_upload(uint64_t sequence,unsigned offset,const void *in,unsigned bytes)
{
 struct i2s_runtime *s=&app.runtime;
 if(!i2s_live(NULL)||!sequence||!in||!bytes||offset>4096||bytes>4096-offset)return -1;
 uint64_t h=atomic_load_explicit(&s->head,memory_order_relaxed);
 if(h>=UINT64_MAX/I2S_RUNTIME_FRAMES||sequence!=h+1||offset!=s->uploaded)return -1;
 if(!i2s_runtime_credits())return 0;
 memcpy(s->bytes[h%I2S_RUNTIME_SLOTS]+offset,in,bytes);s->uploaded+=bytes;
 if(s->uploaded==4096){
  /* Serialize closure with publication; cancellation never exposes partial PCM. */
  taskENTER_CRITICAL();
  int live=i2s_live(NULL)&&!atomic_load_explicit(&s->closed,memory_order_relaxed)&&
      !atomic_load_explicit(&app.cancel,memory_order_relaxed)&&
      atomic_load_explicit(&app.result,memory_order_relaxed)<2;
  if(live)atomic_store_explicit(&s->head,h+1,memory_order_release);
  taskEXIT_CRITICAL();s->uploaded=0;if(!live)return 0;
 }
 return 1;
}
static void closing(unsigned failed)
{
 taskENTER_CRITICAL();atomic_store_explicit(&app.runtime.closed,1,memory_order_release);taskEXIT_CRITICAL();
 if(failed)atomic_store_explicit(&app.runtime.flags,1,memory_order_release);
 app.runtime.stage=STOP_STATUS;
}
static int fault(void)
{closing(1);atomic_store_explicit(&app.result,3,memory_order_release);return -1;}
int i2s_runtime_step(void)
{
 struct i2s_runtime *s=&app.runtime;int r;
 if(atomic_load_explicit(&app.cancel,memory_order_acquire)&&!app.pending&&s->stage>OPEN&&s->stage<STOP_STATUS)closing(0);
 app.job=0;
 switch(s->stage){
 case OPEN:
  atomic_store_explicit(&app.result,1,memory_order_release);
  r=i2s_request(DMA_SERVICE_OPEN,100000,0);if(r<0)return fault();if(!r)return 0;s->stage=WAIT_OPEN;return 0;
 case WAIT_OPEN:
  r=i2s_request(DMA_SERVICE_STATUS,0,0);if(r<0)return fault();if(!r)return 0;
  if(app.reply.phase==DMA_SERVICE_OPENING)return 0;
  if(app.reply.phase!=DMA_SERVICE_READY)return fault();s->stage=PRIME_GET;return 0;
 case PRIME_GET:
 case GET:{
  unsigned prime=s->stage==PRIME_GET;
  r=i2s_request(DMA_SERVICE_PERIOD_GET,0,0);if(r<0)return fault();
  if(!r){if(!app.pending&&app.reply.result==DMA_SERVICE_BUSY)s->stage=CHECK;return 0;}
  struct a733_i2s_period p=app.reply.period;
  uint64_t consumed=atomic_load_explicit(&s->consumed,memory_order_relaxed);
  if(!p.ticket||p.ticket<=s->held||p.slot>=I2S_PERIODS||p.frames!=I2S_RUNTIME_FRAMES||
     p.kind!=(prime?A733_I2S_TX_FILL:A733_I2S_TX_MEMORY_CONSUMED)||
     (prime?p.slot!=s->primed:p.first_frame!=consumed))return fault();
  s->held=p.ticket;s->slot=p.slot;
  if(!prime){if(consumed>UINT64_MAX-p.frames)return fault();
   atomic_store_explicit(&s->consumed,consumed+p.frames,memory_order_release);}
  s->stage=FILL;return 0;}
 case FILL:{
  uint64_t t=atomic_load_explicit(&s->tail,memory_order_relaxed),h=atomic_load_explicit(&s->head,memory_order_acquire);
  if(h<t||h-t>I2S_RUNTIME_SLOTS)return fault();
  if(h==t){s->stage=WAIT_INPUT;return 0;}
  /* Only this original service client holds the exact completed/fill lease. */
  memcpy(app.period_memory[s->slot],s->bytes[t%I2S_RUNTIME_SLOTS],4096);
  atomic_store_explicit(&s->tail,t+1,memory_order_release);s->stage=PUT;return 0;}
 case WAIT_INPUT:
  /* Collect this STATUS under its original operation even if input arrives. */
  r=i2s_request(DMA_SERVICE_STATUS,0,0);if(r<0)return fault();if(!r)return 0;
  if(app.reply.phase==DMA_SERVICE_DRAINING||app.reply.phase==DMA_SERVICE_RESULT)closing(1);
  else if(app.reply.phase!=(s->started?DMA_SERVICE_RUNNING:DMA_SERVICE_READY))return fault();
  else s->stage=FILL;return 0;
 case PUT:
  r=i2s_request(DMA_SERVICE_PERIOD_PUT,0,s->held);if(r<0)return fault();if(!r)return 0;
  {uint64_t n=atomic_load_explicit(&s->committed,memory_order_relaxed);
   if(n>UINT64_MAX-I2S_RUNTIME_FRAMES)return fault();
   atomic_store_explicit(&s->committed,n+I2S_RUNTIME_FRAMES,memory_order_release);}
  s->stage=s->started?GET:++s->primed==I2S_PERIODS?START:PRIME_GET;return 0;
 case START:
  r=i2s_request(DMA_SERVICE_SUBMIT,24000000,0);if(r<0)return fault();if(!r)return 0;
  s->started=1;s->stage=GET;return 0;
 case CHECK:
  r=i2s_request(DMA_SERVICE_STATUS,0,0);if(r<0)return fault();if(!r)return 0;
  if(app.reply.phase==DMA_SERVICE_DRAINING||app.reply.phase==DMA_SERVICE_RESULT){closing(1);return 0;}
  if(app.reply.phase!=(s->started?DMA_SERVICE_RUNNING:DMA_SERVICE_READY))return fault();
  s->stage=s->started?GET:PRIME_GET;return 0;
 case STOP_STATUS:
  r=i2s_request(DMA_SERVICE_STATUS,0,0);if(r<0)return fault();if(!r)return 0;
  if(app.reply.phase==DMA_SERVICE_RUNNING||app.reply.phase==DMA_SERVICE_READY){s->stage=STOP_CANCEL;return 0;}
  if(app.reply.phase==DMA_SERVICE_DRAINING||app.reply.phase==DMA_SERVICE_OPENING)return 0;
  if(app.reply.phase!=DMA_SERVICE_RESULT||!app.reply.holds_buffers||
     (app.reply.outcome!=DMA_SERVICE_CANCELED&&app.reply.outcome!=DMA_SERVICE_FAILED))return fault();
  if(app.reply.outcome==DMA_SERVICE_FAILED)atomic_store_explicit(&s->flags,1,memory_order_release);
  atomic_store_explicit(&s->drained,1,memory_order_release);
  {unsigned failed=atomic_load_explicit(&s->flags,memory_order_acquire);
   atomic_store_explicit(&app.result,failed?3:2,memory_order_release);return failed?-1:1;}
 case STOP_CANCEL:
  r=i2s_request(DMA_SERVICE_CANCEL,0,0);if(r<0)return fault();if(!r&&app.pending)return 0;
  s->stage=STOP_STATUS;return 0;
 default:return fault();
 }
}
