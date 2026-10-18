#include "recognition.h"
#include "../mimus/recorded.h"
extern "C" {
#include "a733_storage_digest.h"
#include "runtime.h"
#include "FreeRTOS.h"
#include "task.h"
void runtime_smp_init(void);
}
#include <atomic>
#include <cstring>
#include <span>
namespace {
struct Retained {
 i2s_models models;StaticTask_t task;StackType_t stack[16384];TaskHandle_t owner;
 alignas(64) uint8_t scratch[97280];
 alignas(4) uint8_t wav[44+I2S_CAPTURE_MAX_BYTES];
 uint8_t text[I2S_RECOGNITION_LOG];unsigned used;
 i2s_recognition_snapshot snapshot;
} retained;
std::atomic<unsigned> terminal{0};
std::atomic<bool> stopped{false};
bool live(){return !stopped.load(std::memory_order_acquire)&&i2s_capture_valid()>=0;}
void fail(){stopped.store(true,std::memory_order_release);terminal.store(3,std::memory_order_release);}
void put16(uint8_t *p,unsigned v){p[0]=v;p[1]=v>>8;}
void put32(uint8_t *p,unsigned v){for(unsigned i=0;i<4;++i)p[i]=v>>(8*i);}
bool model(unsigned i)
{
 storage_digest digest;storage_digest_init(&digest);
 const uint8_t *data=reinterpret_cast<const uint8_t *>(I2S_MODEL_BASE+i*I2S_MODEL_SLOT);
 for(uint64_t offset=0;offset<retained.models.bytes[i];){
  if(!live())return false;
  size_t count=retained.models.bytes[i]-offset;if(count>4096)count=4096;
  if(!storage_digest_add(&digest,data+offset,count))return false;offset+=count;
 }
 uint8_t hash[32];storage_digest_end(&digest,hash);
 return live()&&!std::memcmp(hash,retained.models.sha256[i],32);
}
void infer(void *)
{
 while(live()&&i2s_capture_valid()==0)vTaskDelay(1);
 unsigned bytes=i2s_capture_bytes();bool success=false;
 if(i2s_capture_valid()==1&&bytes>=2*I2S_CAPTURE_MIN_SAMPLES&&bytes<=I2S_CAPTURE_MAX_BYTES&&
    !(bytes&1)&&i2s_capture_copy(retained.wav+44,bytes)&&model(0)&&model(1)){
  auto *wav=retained.wav;std::memcpy(wav,"RIFF",4);put32(wav+4,36+bytes);
  std::memcpy(wav+8,"WAVEfmt ",8);put32(wav+16,16);put16(wav+20,1);put16(wav+22,1);
  put32(wav+24,16000);put32(wav+28,32000);put16(wav+32,2);put16(wav+34,16);
  std::memcpy(wav+36,"data",4);put32(wav+40,bytes);
  storage_digest_bytes(wav+44,bytes,retained.snapshot.pcm_sha256);
  std::span<const uint8_t> inputs[3]={
   {reinterpret_cast<const uint8_t *>(I2S_MODEL_BASE),static_cast<size_t>(retained.models.bytes[0])},
   {reinterpret_cast<const uint8_t *>(I2S_MODEL_BASE+I2S_MODEL_SLOT),static_cast<size_t>(retained.models.bytes[1])},
   {wav,44+bytes}};
  // Return includes destruction of the unchanged engine and temporary objects.
  success=live()&&mimus_transcribe_recorded(inputs,retained.scratch,&stopped);
 }
 if(success&&live()&&i2s_capture_valid()==1&&retained.used){
  retained.snapshot.bytes=retained.used;retained.snapshot.samples=bytes/2;
  storage_digest_bytes(retained.text,retained.used,retained.snapshot.text_sha256);
  if(live())terminal.store(2,std::memory_order_release);else fail();
 }else fail();
 for(;;)vTaskSuspend(nullptr);
}
bool separate(const void *p,size_t bytes,const void *q,size_t count)
{
 uintptr_t a=(uintptr_t)p,b=(uintptr_t)q;
 return p&&q&&bytes<=UINTPTR_MAX-a&&count<=UINTPTR_MAX-b&&(a+bytes<=b||b+count<=a);
}
}
extern "C" void platform_puts(const char *text)
{
 if(xTaskGetCurrentTaskHandle()!=retained.owner||!text||!live()){fail();return;}
 size_t count=0,available=sizeof(retained.text)-retained.used;
 while(count<available&&text[count])++count;
 if(count==available){fail();return;}
 std::memcpy(retained.text+retained.used,text,count);retained.used+=count;
}
extern "C" void platform_hex(uint64_t value)
{
 static const char hex[]="0123456789abcdef";char text[17];
 for(unsigned i=0;i<16;++i)text[i]=hex[(value>>(60-4*i))&15];text[16]=0;platform_puts(text);
}
extern "C" int i2s_recognition_start(const i2s_models *models)
{
 if(!i2s_models_valid(models)||retained.owner||xTaskGetSchedulerState()!=taskSCHEDULER_NOT_STARTED)return 0;
 retained.models=*models;runtime_smp_init();runtime_init();
 retained.owner=xTaskCreateStatic(infer,"i2s-mimus",16384,nullptr,3,retained.stack,&retained.task);
 return retained.owner!=nullptr;
}
extern "C" void i2s_recognition_abort(void){fail();}
extern "C" void i2s_recognition_poll(void){if(i2s_capture_valid()<0)fail();}
extern "C" int i2s_recognition_copy(unsigned offset,unsigned bytes,void *out,i2s_recognition_snapshot *snapshot)
{
 if(!out||!snapshot||bytes>4096||!separate(out,bytes,snapshot,sizeof(*snapshot)))return -1;
 const void *destinations[]={out,snapshot};size_t sizes[]={bytes,sizeof(*snapshot)};
 for(unsigned i=0;i<2;++i)if(!i2s_copy_target(destinations[i],sizes[i])||
    !separate(destinations[i],sizes[i],&retained,sizeof(retained))||
    !separate(destinations[i],sizes[i],reinterpret_cast<const void *>(I2S_MODEL_BASE),2*I2S_MODEL_SLOT)||
    !separate(destinations[i],sizes[i],reinterpret_cast<const void *>(0x46000000),16*1024*1024))return -1;
 unsigned state=terminal.load(std::memory_order_acquire);
 if(!live()||state==3)return -1;if(state!=2)return 0;
 if(offset>retained.used||bytes>retained.used-offset)return -1;
 std::memcpy(out,retained.text+offset,bytes);*snapshot=retained.snapshot;
 return live()&&i2s_capture_valid()==1?1:-1;
}
