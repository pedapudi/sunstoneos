#include "runtime.h"
#include "boot.h"
#include "result.h"
#include <string.h>
struct npu_runtime_slot npu_runtime;
uint8_t npu_runtime_input[524288] __attribute__((section(".npu_graph_input"),aligned(64)));
uint8_t npu_runtime_output[262144] __attribute__((section(".npu_graph_output"),aligned(64)));
__attribute__((used,retain)) const uint64_t npu_runtime_geometry[6]={
 NPU_RUNTIME_FRAME,2,64,NPU_RUNTIME_CHUNK,522240,262144};
static uint64_t last_id,last_time,read_offset;
static unsigned received,delivered,chunk_remaining,read_requested,finishing;
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
int npu_runtime_header(const uint8_t p[64])
{
 if(!current())return -1;
 unsigned op=get32(p+8),length=get32(p+12),bytes=get32(p+48);
 uint64_t id=get64(p+32),us=get64(p+40),offset=get64(p+56);
 if(get32(p)!=A733_OBSERVE_MESSAGE_MAGIC||length!=48||get64(p+16)!=a733_observe_identity.epoch||
  get64(p+24)!=a733_observe_identity.generation||get32(p+52)||!id){npu_runtime_fail(31);return -1;}
 unsigned state=atomic_load_explicit(&npu_runtime.state,memory_order_acquire);
 read_requested=0;
 if(op==NPU_RUNTIME_BEGIN||op==NPU_RUNTIME_CLOSE){
  if(state!=NR_IDLE||id<=last_id||!us||us>5000000||offset||
   bytes!=(op==NPU_RUNTIME_BEGIN?a733_npu_application_input_bytes:0))goto malformed;
  uint64_t now=a733_smp_counter(),span=us*24;
  if(now>=a733_observe_identity.deadline)goto malformed;
  if(span>a733_observe_identity.deadline-now)span=a733_observe_identity.deadline-now;
  npu_runtime.id=last_id=id;npu_runtime.deadline=now+span;
  npu_runtime.operation=op==NPU_RUNTIME_CLOSE?NPU_RUNTIME_CLOSE:NPU_RUNTIME_BEGIN;
  npu_runtime.bytes=bytes;npu_runtime.job=0;npu_runtime.start=npu_runtime.end=0;
  received=delivered=chunk_remaining=finishing=0;read_offset=0;
  memset(npu_runtime_input,0,sizeof(npu_runtime_input));memset(npu_runtime_output,0,sizeof(npu_runtime_output));
  memset(npu_runtime.input_digest,0,32);memset(npu_runtime.output_digest,0,32);
  if(!current()||a733_smp_counter()>=npu_runtime.deadline){npu_runtime_fail(34);return -1;}
  atomic_store_explicit(&npu_runtime.state,bytes?NR_UPLOAD:NR_QUEUED,memory_order_release);return 0;
 }
 if(id!=npu_runtime.id||us||chunk_remaining||finishing)goto malformed;
 if(op==NPU_RUNTIME_UPLOAD){
  if(state!=NR_UPLOAD||!bytes||bytes>NPU_RUNTIME_CHUNK||offset!=received||
   bytes>npu_runtime.bytes-received)goto malformed;
  chunk_remaining=bytes;return (int)bytes;
 }
 if(op==NPU_RUNTIME_COMMIT){
  if(state!=NR_UPLOAD||bytes||offset||received!=npu_runtime.bytes)goto malformed;
  a733_observe_digest(npu_runtime_input,received,npu_runtime.input_digest);
  if(!current())return -1;
  atomic_store_explicit(&npu_runtime.state,NR_QUEUED,memory_order_release);return 0;
 }
 if(op==NPU_RUNTIME_READ){
  if(bytes||state==NR_IDLE||state==NR_UPLOAD||
   (state==NR_DONE?(offset!=delivered||offset>=a733_npu_application_reference_bytes):offset!=0))goto malformed;
  read_requested=1;read_offset=offset;return 0;
 }
 if(op==NPU_RUNTIME_FINISH){
  if(state!=NR_DONE||bytes||offset||delivered!=a733_npu_application_reference_bytes)goto malformed;
  finishing=1;return 0;
 }
malformed:
 npu_runtime_fail(33);return -1;
}
int npu_runtime_payload(const uint8_t *p,unsigned bytes)
{
 if(!current()||atomic_load_explicit(&npu_runtime.state,memory_order_acquire)!=NR_UPLOAD||
  !p||!bytes||bytes>chunk_remaining||bytes>npu_runtime.bytes-received){npu_runtime_fail(35);return -1;}
 memcpy(npu_runtime_input+received,p,bytes);received+=bytes;chunk_remaining-=bytes;
 return (int)chunk_remaining;
}
int npu_runtime_frame(uint8_t p[1024],unsigned *ack,uint64_t *id,uint64_t *offset)
{
 if(!p||!ack||!id||!offset)return 0;
 (void)current();unsigned state=atomic_load_explicit(&npu_runtime.state,memory_order_acquire);
 memset(p,0,1024);put64(p,NPU_RUNTIME_FRAME);put64(p+8,UINT64_C(1024)<<32|2);
 put64(p+16,state);put64(p+24,a733_observe_identity.epoch);put64(p+32,a733_observe_identity.generation);
 put64(p+40,a733_observe_identity.lease);memcpy(p+48,a733_smp_build_id,32);
 put64(p+80,npu_runtime.id);put64(p+88,npu_runtime.deadline);
 memcpy(p+96,a733_npu_application_artifact_hashes[0],32);*ack=0;*id=npu_runtime.id;*offset=0;
 if(state==NR_DONE||state==NR_CLOSED){
  put64(p+128,npu_runtime.start);put64(p+136,npu_runtime.end);put64(p+144,npu_runtime.job);
  put64(p+152,npu_runtime.jobs);put64(p+160,npu_runtime.irq_count);
  if(state==NR_DONE){
   memcpy(p+168,npu_runtime.input_digest,32);memcpy(p+200,npu_runtime.output_digest,32);
   put64(p+232,a733_npu_application_reference_bytes);
   if(read_requested&&read_offset==delivered&&delivered<a733_npu_application_reference_bytes){
    unsigned n=(unsigned)a733_npu_application_reference_bytes-delivered;if(n>NPU_RUNTIME_CHUNK)n=NPU_RUNTIME_CHUNK;
    put64(p+240,delivered);put64(p+248,n);memcpy(p+256,npu_runtime_output+delivered,n);
    *ack=NR_DONE;*offset=delivered;
   }else if(finishing){*ack=NR_ACK_FINISH;}
  }
 }
 put64(p+768,atomic_load_explicit(&npu_runtime.reason,memory_order_acquire));
 put64(p+776,npu_runtime.bytes);put64(p+784,received);put64(p+792,delivered);
 a733_observe_digest(p,992,p+992);return 1;
}
void npu_runtime_ack(unsigned state,uint64_t id,uint64_t offset)
{
 if(!state)return;
 if(!current()||id!=npu_runtime.id||atomic_load_explicit(&npu_runtime.state,memory_order_acquire)!=NR_DONE){npu_runtime_fail(36);return;}
 if(state==NR_DONE){
  if(finishing||!read_requested||offset!=read_offset||offset!=delivered||delivered>=a733_npu_application_reference_bytes){npu_runtime_fail(36);return;}
  unsigned n=(unsigned)a733_npu_application_reference_bytes-delivered;if(n>NPU_RUNTIME_CHUNK)n=NPU_RUNTIME_CHUNK;
  delivered+=n;read_requested=0;
 }else if(state==NR_ACK_FINISH){
  unsigned expected=NR_DONE;
  if(offset||!finishing||delivered!=a733_npu_application_reference_bytes||
   !atomic_compare_exchange_strong_explicit(&npu_runtime.state,&expected,NR_IDLE,memory_order_release,memory_order_relaxed))npu_runtime_fail(36);
 }else npu_runtime_fail(36);
}
