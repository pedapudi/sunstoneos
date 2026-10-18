#include "storage_read.h"
#include "a733_storage_digest.h"
#include <string.h>
struct storage_read_consumer storage_read_consumer;
static uint32_t get32(const uint8_t *p)
{return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static uint64_t get64(const uint8_t *p){return get32(p)|(uint64_t)get32(p+4)<<32;}
static void fail(void)
{
 if(storage_read_consumer.ticket)storage_read_consumer.stage=3;
 else atomic_store_explicit(&storage_read_consumer.result,2,memory_order_release);
}
static void step(void)
{
 struct storage_read_consumer *c=&storage_read_consumer;
 if(c->stage==3){
  if(storage_read_cancel(c->ticket)!=0)atomic_store_explicit(&c->result,2,memory_order_release);
  return;
 }
 if(c->stage==0){
  struct storage_read_request request={.first_sector=storage_read.session.application_first_sector,
   .blocks=storage_read.session.application_blocks,.request_id=(uint64_t)atomic_load(&c->completed)+1};
  int r=storage_read_submit(&request,&c->ticket);
  if(r==1)c->stage=1;else if(r<0)fail();return;
 }
 if(c->stage==1){
  unsigned bytes;
  if(!storage_read_copy(c->response,sizeof(c->response),&bytes))return;
  unsigned status=get32(c->response+8);
  if(status==STORAGE_READ_PENDING)return;
  unsigned count=storage_read.session.application_blocks*512u;
  if(status!=STORAGE_READ_SUCCESS||bytes!=STORAGE_READ_HEADER+count||get32(c->response+12)!=count||
     get64(c->response+240)!=c->ticket||get64(c->response+248)!=storage_read.session.application_first_sector||
     get64(c->response+256)!=(uint64_t)atomic_load(&c->completed)+1||
     get32(c->response+264)!=storage_read.session.application_blocks||get32(c->response+268)!=1){fail();return;}
  storage_digest_bytes(c->response+STORAGE_READ_HEADER,count,c->digest);
  if(memcmp(c->digest,c->response+32,32)){fail();return;}
  c->stage=2;
 }
 if(c->stage==2){
  if(!storage_read_ack(c->ticket)){
   if(!storage_read_output_ready())fail();return;
  }
  c->ticket=0;c->stage=0;
  if(atomic_fetch_add_explicit(&c->completed,1,memory_order_acq_rel)+1==2)
   atomic_store_explicit(&c->result,1,memory_order_release);
 }
}
void storage_read_client_task(void *unused)
{
 (void)unused;
 for(;;){if(!atomic_load_explicit(&storage_read_consumer.result,memory_order_acquire))step();vTaskDelay(1);}
}
