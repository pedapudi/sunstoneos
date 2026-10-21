#include "runtime.h"
#include "boot.h"
#include "result.h"
#include <string.h>
struct npu_runtime_slot npu_runtime;
__attribute__((used,retain)) const uint64_t npu_runtime_geometry[4]={NPU_RUNTIME_FRAME,1,64,NPU_CONVOLUTION_IO_CAPACITY};
static uint64_t last_id,last_time;
static unsigned received;
static uint32_t get32(const uint8_t *p)
{return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static uint64_t get64(const uint8_t *p)
{return get32(p)|(uint64_t)get32(p+4)<<32;}
static void put64(uint8_t *p,uint64_t v)
{for(unsigned i=0;i<8;++i)p[i]=(uint8_t)(v>>(8*i));}
int npu_runtime_live(void)
{
 uint64_t now=a733_smp_counter();
 return !atomic_load_explicit(&a733_npu_application_failed,memory_order_acquire)&&
  now>=a733_observe_identity.armed_at&&now<a733_observe_identity.deadline;
}
void npu_runtime_fail(unsigned reason)
{
 unsigned zero=0;atomic_compare_exchange_strong_explicit(&npu_runtime.reason,&zero,reason,memory_order_release,memory_order_relaxed);
 atomic_store_explicit(&npu_runtime.state,NR_FAILED,memory_order_release);
 a733_npu_application_fail(reason);
}
static int current(void)
{
 uint64_t now=a733_smp_counter();unsigned state=atomic_load_explicit(&npu_runtime.state,memory_order_acquire);
 if(now<last_time||!npu_runtime_live()||
  (state!=NR_IDLE&&state!=NR_CLOSED&&state!=NR_FAILED&&now>=npu_runtime.deadline)){
  npu_runtime_fail(30);return 0;
 }
 last_time=now;return state!=NR_FAILED;
}
/* USB owner poll: latch request expiry without building a response frame. */
void npu_runtime_expire(void)
{(void)current();}
int npu_runtime_header(const uint8_t p[64])
{
 if(!current())return -1;
 unsigned op=get32(p+8),length=get32(p+12),bytes=get32(p+48);
 uint64_t id=get64(p+32),us=get64(p+40);
 if(get32(p)!=A733_OBSERVE_MESSAGE_MAGIC||length!=48||get64(p+16)!=a733_observe_identity.epoch||
  get64(p+24)!=a733_observe_identity.generation||get32(p+52)||get64(p+56)||!id){npu_runtime_fail(31);return -1;}
 unsigned state=atomic_load_explicit(&npu_runtime.state,memory_order_acquire);
 if(op==NPU_RUNTIME_READ){
  if(bytes||us||id!=npu_runtime.id||state==NR_IDLE||state==NR_UPLOAD){npu_runtime_fail(32);return -1;}
  return 0;
 }
 if((op!=NPU_RUNTIME_SUBMIT&&op!=NPU_RUNTIME_CLOSE)||state!=NR_IDLE||id<=last_id||
  !us||us>5000000|| (op==NPU_RUNTIME_SUBMIT?bytes!=a733_npu_application_input_bytes:bytes!=0)){
  npu_runtime_fail(33);return -1;
 }
 uint64_t now=a733_smp_counter(),span=us*24;
 if(now>=a733_observe_identity.deadline){npu_runtime_fail(34);return -1;}
 if(span>a733_observe_identity.deadline-now)span=a733_observe_identity.deadline-now;
 npu_runtime.id=last_id=id;npu_runtime.deadline=now+span;npu_runtime.operation=op;
 npu_runtime.bytes=bytes;npu_runtime.job=0;npu_runtime.start=npu_runtime.end=0;received=0;
 memset(npu_runtime.input,0,sizeof(npu_runtime.input));memset(npu_runtime.output,0,sizeof(npu_runtime.output));
 memset(npu_runtime.input_digest,0,32);memset(npu_runtime.output_digest,0,32);
 atomic_store_explicit(&npu_runtime.state,bytes?NR_UPLOAD:NR_QUEUED,memory_order_release);
 return (int)bytes;
}
int npu_runtime_payload(const uint8_t *p,unsigned bytes)
{
 if(!current()||atomic_load_explicit(&npu_runtime.state,memory_order_acquire)!=NR_UPLOAD||
  !p||!bytes||bytes>npu_runtime.bytes-received){npu_runtime_fail(35);return -1;}
 memcpy(npu_runtime.input+received,p,bytes);received+=bytes;
 if(received==npu_runtime.bytes){
  a733_observe_digest(npu_runtime.input,received,npu_runtime.input_digest);
  if(!current())return -1;
  unsigned expected=NR_UPLOAD;
  if(!atomic_compare_exchange_strong_explicit(&npu_runtime.state,&expected,NR_QUEUED,memory_order_release,memory_order_relaxed))return -1;
 }
 return (int)(npu_runtime.bytes-received);
}
int npu_runtime_frame(uint8_t out[1024],unsigned *ack,uint64_t *id)
{
 (void)current();unsigned state=atomic_load_explicit(&npu_runtime.state,memory_order_acquire);
 memset(out,0,1024);put64(out,NPU_RUNTIME_FRAME);put64(out+8,UINT64_C(1024)<<32|1);
 put64(out+16,state);put64(out+24,a733_observe_identity.epoch);put64(out+32,a733_observe_identity.generation);
 put64(out+40,a733_observe_identity.lease);memcpy(out+48,a733_smp_build_id,32);
 put64(out+80,npu_runtime.id);put64(out+88,npu_runtime.deadline);
 memcpy(out+96,a733_npu_application_artifact_hashes[0],32);
 /* RUNNING fields belong to the client until its release publication. */
 *ack=0;*id=npu_runtime.id;
 if(state==NR_DONE||state==NR_CLOSED){
  put64(out+128,npu_runtime.start);put64(out+136,npu_runtime.end);put64(out+144,npu_runtime.job);
  put64(out+152,npu_runtime.jobs);put64(out+160,npu_runtime.irq_count);
  memcpy(out+168,npu_runtime.input_digest,32);memcpy(out+200,npu_runtime.output_digest,32);
  unsigned count=state==NR_DONE?(unsigned)a733_npu_application_reference_bytes:0;
  put64(out+232,count);memcpy(out+240,npu_runtime.output,count);*ack=state;
 }
 put64(out+752,atomic_load_explicit(&npu_runtime.reason,memory_order_acquire));
 if(!npu_runtime_live()||atomic_load_explicit(&npu_runtime.state,memory_order_acquire)==NR_FAILED){
  memset(out+128,0,624);put64(out+16,NR_FAILED);*ack=0;
 }
 a733_observe_digest(out,992,out+992);return 1;
}
void npu_runtime_ack(unsigned state,uint64_t id)
{
 if(!state)return;
 if(!current()||id!=npu_runtime.id){npu_runtime_fail(36);return;}
 if(state==NR_DONE){unsigned expected=NR_DONE;
  if(!atomic_compare_exchange_strong_explicit(&npu_runtime.state,&expected,NR_IDLE,memory_order_acq_rel,memory_order_acquire))npu_runtime_fail(36);
 }
 /* CLOSED is terminal and remains queryable until the session ends. */
}
