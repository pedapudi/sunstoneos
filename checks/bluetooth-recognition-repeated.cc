// Actual repeated bridge and unchanged engine. Scheduling and the borrowed
// segment source are modeled.
#include "../apps/bluetooth/recognition.h"
extern "C" {
#include "../apps/bluetooth/headset.h"
#include "../apps/bluetooth/pcm.h"
#include "a733_storage_digest.h"
}
#include "FreeRTOS.h"
#include "task.h"
#include "mimus/asr/platform.h"
#include <cassert>
#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include <sys/mman.h>
static std::jmp_buf parked;
static TaskFunction_t worker;
static TaskHandle_t task=reinterpret_cast<TaskHandle_t>(1);
static bluetooth_application *app=reinterpret_cast<bluetooth_application *>(0x10000);
static bool scheduler_started,revoked;
static unsigned consumed,outputs,stops,requests,wakes;
static uint64_t clock_us=1;
static std::vector<uint8_t> pcm;
static bluetooth_segment_snapshot identities[2];
static const char *prefix;
namespace mimus::asr::platform {uint64_t MonotonicNanos(){return clock_us*1000;}uintptr_t TaskIdentity(){return 1;}}
extern "C" TaskHandle_t xTaskGetCurrentTaskHandle(){return task;}
extern "C" BaseType_t xTaskGetSchedulerState(){return scheduler_started?taskSCHEDULER_RUNNING:taskSCHEDULER_NOT_STARTED;}
extern "C" TaskHandle_t xTaskCreateStatic(TaskFunction_t fn,const char *const name,const configSTACK_DEPTH_TYPE depth,
 void *const context,UBaseType_t priority,StackType_t *const stack,StaticTask_t *const storage)
{assert(fn&&name&&depth==16384&&!context&&priority==3&&stack&&storage);worker=fn;return task;}
extern "C" void vTaskSuspend(TaskHandle_t){std::longjmp(parked,1);}
extern "C" void fixture_task_yield(){}
extern "C" void vTaskYieldWithinAPI(){}
extern "C" void runtime_smp_init(){assert(!scheduler_started);}
extern "C" void runtime_init(){assert(!scheduler_started);}
extern "C" size_t runtime_heap_peak(){return 0;}
extern "C" int bluetooth_recognition_live(bluetooth_application *p){assert(p==app);return !revoked;}
extern "C" int bluetooth_recognition_output(bluetooth_application *p,const void *out,size_t bytes){assert(p==app&&out&&bytes);return 1;}
extern "C" int bluetooth_headset_wideband(const bluetooth_application *p){assert(p==app);
#ifdef BLUETOOTH_CHECK_MSBC
 return 1;
#else
 return 0;
#endif
}
extern "C" int bluetooth_headset_repeated(const bluetooth_application *p){assert(p==app);return 1;}
extern "C" int bluetooth_headset_segment_bind(bluetooth_application *p,void *consumer){assert(p==app&&consumer==task&&!scheduler_started);return 1;}
extern "C" int bluetooth_headset_segment_stop(bluetooth_application *p){assert(p==app);++stops;return 1;}
// Each request records one segment; the headset disconnects after two.
static unsigned published(){return requests<2?requests:2;}
extern "C" int bluetooth_headset_segment_status(bluetooth_application *p,bluetooth_segment_status *s)
{assert(p==app);*s={published(),consumed,published()==2?BLUETOOTH_SEGMENT_CANCEL:0u,published()==2};return 1;}
// The recognizer borrows each published segment in place.
extern "C" int bluetooth_headset_segment_take(bluetooth_application *p,const uint8_t **out,bluetooth_segment_snapshot *s)
{
 assert(p==app&&out&&s);*out=nullptr;std::memset(s,0,sizeof(*s));if(consumed==published())return 0;
 *s=identities[consumed];*out=pcm.data()+consumed*48000;return 1;
}
// The first request starts an activation; later ones admit the button.
// The recognizer passes the head its take saw, which is the consumed count.
extern "C" int bluetooth_headset_segment_request(bluetooth_application *p,unsigned word,uint64_t observed)
{assert(p==app&&word==(requests?BLUETOOTH_SEGMENT_ACCEPT:BLUETOOTH_SEGMENT_START)&&observed==consumed);++requests;return 1;}
extern "C" int bluetooth_headset_segment_ack(bluetooth_application *p,uint64_t ticket)
{assert(p==app&&ticket==consumed+1&&consumed<published());++consumed;return 1;}
extern "C" int bluetooth_headset_copy(bluetooth_application *,unsigned,uint32_t,size_t,uint8_t[1024],size_t *,bluetooth_pcm_snapshot *)
{assert(false);return -1;}
static void write(const std::string &name,const void *data,size_t bytes)
{FILE *f=std::fopen(name.c_str(),"wb");assert(f&&std::fwrite(data,1,bytes,f)==bytes&&!std::fclose(f));}
extern "C" void vTaskDelay(TickType_t){assert(false);}
extern "C" BaseType_t xTaskNotifyGive(TaskHandle_t t){assert(t==reinterpret_cast<TaskHandle_t>(1));++wakes;return 1;}
// Each wait of the recognizer runs the USB consumer once.
extern "C" uint32_t ulTaskNotifyTake(BaseType_t clear,TickType_t ticks)
{
 assert(clear==pdTRUE&&ticks==pdMS_TO_TICKS(100));
 ++clock_us;uint8_t chunk[1024];size_t copied;bluetooth_repeated_snapshot s{};
 task=reinterpret_cast<TaskHandle_t>(2);
 int r=bluetooth_recognition_segment_copy(app,0,0,0,chunk,&copied,&s);
 if(r==1){
  assert(s.ticket==outputs+1&&s.first_sample==outputs*24000&&consumed==outputs+1);
  assert(s.result.input_samples==24000&&s.result.output_samples==(bluetooth_headset_wideband(app)?24000u:48000u));
  assert(!std::memcmp(s.result.pcm_sha256,identities[outputs].capture.sha256,32));
  std::vector<uint8_t> text;
  for(uint32_t off=0;off<s.result.bytes;){size_t n=s.result.bytes-off;if(n>1024)n=1024;
   bluetooth_repeated_snapshot next{};
   assert(bluetooth_recognition_segment_copy(app,s.ticket,off,n,chunk,&copied,&next)==1&&copied==n&&
      !std::memcmp(&s,&next,sizeof(s)));text.insert(text.end(),chunk,chunk+n);off+=static_cast<uint32_t>(n);}
  uint8_t digest[32];storage_digest_bytes(text.data(),text.size(),digest);assert(!std::memcmp(digest,s.result.text_sha256,32));
  // The first segment loads the retained engine; later segments reuse it.
  assert((std::string(text.begin(),text.end()).find("LOAD load_us=")!=std::string::npos)==(s.ticket==1));
  const std::string stem=std::string(prefix)+"-"+std::to_string(s.ticket);
  write(stem+".txt",text.data(),text.size());write(stem+".snapshot.bin",&s,sizeof(s));
  uint8_t wav[96044];size_t bytes;
#ifdef BLUETOOTH_CHECK_MSBC
  assert(bluetooth_pcm16_wav(pcm.data()+outputs*48000,48000,wav,sizeof(wav),&bytes)==1&&s.result.reserved==2);
#else
  assert(bluetooth_pcm_wav(pcm.data()+outputs*48000,48000,wav,sizeof(wav),&bytes)==1);
#endif
  write(stem+".wav",wav,bytes);
  assert(bluetooth_recognition_segment_ack(app,s.ticket+1)==-1);
  assert(bluetooth_recognition_segment_ack(app,s.ticket)==1&&bluetooth_recognition_segment_ack(app,s.ticket)==-1);++outputs;
 }
 task=reinterpret_cast<TaskHandle_t>(1);return 0;
}
static std::vector<uint8_t> read(const char *name)
{std::ifstream f(name,std::ios::binary);assert(f);return {std::istreambuf_iterator<char>(f),{}};}
int main(int argc,char **argv)
{
 assert(argc==6);pcm=read(argv[1]);assert(pcm.size()==96000);auto metadata=read(argv[2]);assert(metadata.size()==sizeof(identities));
 std::memcpy(identities,metadata.data(),sizeof(identities));prefix=argv[5];
 bluetooth_recognition_profile models{};
 for(unsigned i=0;i<2;++i){auto data=read(argv[3+i]);models.bytes[i]=data.size();
  void *at=mmap(reinterpret_cast<void *>(BLUETOOTH_MODEL_BASE+i*BLUETOOTH_MODEL_SLOT),BLUETOOTH_MODEL_SLOT,
   PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);assert(at!=MAP_FAILED);
  std::memcpy(at,data.data(),data.size());storage_digest_bytes(data.data(),data.size(),models.sha256[i]);}
 assert(mmap(reinterpret_cast<void *>(0x46000000),16*1024*1024,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0)!=MAP_FAILED);
 assert(bluetooth_recognition_start(app,&models,60000000)==1&&worker);scheduler_started=true;
 if(!setjmp(parked))worker(nullptr);
 assert(outputs==2&&consumed==2&&!stops&&requests==2&&wakes==2&&bluetooth_recognition_finished(app)==1);
 uint8_t chunk[1024];size_t copied=99;bluetooth_repeated_snapshot value{};revoked=true;
 assert(bluetooth_recognition_segment_copy(app,0,0,0,chunk,&copied,&value)==-1&&!copied);
 std::puts("PASS two borrowed segments through unchanged converter/recognizer, held logs, exact ACK and final revocation");
}
