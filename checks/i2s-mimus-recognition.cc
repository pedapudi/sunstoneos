#include "../apps/i2s/recognition.cc"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <sys/mman.h>
#include <vector>
namespace {unsigned mode,calls,initialized;bool healthy=true;TaskFunction_t entry;struct Done{};}
extern "C" int i2s_capture_valid(void){return healthy?1:-1;}
extern "C" unsigned i2s_capture_bytes(void){return 160000;}
extern "C" int i2s_capture_copy(void *out,unsigned bytes)
{assert(bytes==160000);for(unsigned i=0;i<bytes;++i)static_cast<uint8_t *>(out)[i]=i*7;return 1;}
extern "C" int i2s_copy_target(const void *,size_t){return 1;}
extern "C" void runtime_smp_init(void){assert(!initialized);initialized=1;}
extern "C" void runtime_init(void){assert(initialized==1);initialized=2;}
extern "C" BaseType_t xTaskGetSchedulerState(void){return taskSCHEDULER_NOT_STARTED;}
extern "C" TaskHandle_t xTaskGetCurrentTaskHandle(void){return reinterpret_cast<TaskHandle_t>(1);}
extern "C" TaskHandle_t xTaskCreateStatic(TaskFunction_t fn,const char *,configSTACK_DEPTH_TYPE depth,void *,UBaseType_t priority,StackType_t *,StaticTask_t *)
{assert(initialized==2&&depth==16384&&priority==3);entry=fn;return reinterpret_cast<TaskHandle_t>(1);}
extern "C" void vTaskDelay(TickType_t){assert(false);}
extern "C" void vTaskSuspend(TaskHandle_t){throw Done{};}
bool mimus_transcribe_recorded(std::span<const uint8_t> inputs[3],std::span<uint8_t> scratch,const std::atomic<bool> *stop)
{
 ++calls;assert(inputs[0].data()==reinterpret_cast<const uint8_t *>(I2S_MODEL_BASE)&&inputs[0].size()==7501968);
 assert(inputs[1].data()==reinterpret_cast<const uint8_t *>(I2S_MODEL_BASE+I2S_MODEL_SLOT)&&inputs[1].size()==6560592);
 assert(inputs[2].size()==160044&&!std::memcmp(inputs[2].data(),"RIFF",4)&&scratch.size()==97280);
 const uint8_t *wav=inputs[2].data();assert(wav[24]==0x80&&wav[25]==0x3e&&wav[22]==1&&wav[34]==16);
 for(unsigned i=0;i<160000;++i)assert(wav[44+i]==static_cast<uint8_t>(i*7));
 assert(stop&&!stop->load());
 platform_puts("TRANSCRIPT finite utterance.\n");
 if(mode==2)i2s_recognition_abort();
 if(mode==3){std::vector<char> excess(65537,'x');excess.back()=0;platform_puts(excess.data());}
 if(mode==4)healthy=false;
 return true;
}
int main(int argc,char **argv)
{
 assert(argc==4);mode=std::atoi(argv[1]);
 void *mapped=mmap(reinterpret_cast<void *>(I2S_MODEL_BASE),2*I2S_MODEL_SLOT,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0);
 assert(mapped==reinterpret_cast<void *>(I2S_MODEL_BASE));i2s_models models{};
 for(unsigned i=0;i<2;++i){
  std::ifstream source(argv[2+i],std::ios::binary);std::vector<char> bytes((std::istreambuf_iterator<char>(source)),{});
  assert(bytes.size()==(i?6560592:7501968));models.bytes[i]=bytes.size();
  auto *dest=reinterpret_cast<uint8_t *>(I2S_MODEL_BASE+i*I2S_MODEL_SLOT);std::memcpy(dest,bytes.data(),bytes.size());
  storage_digest_bytes(dest,bytes.size(),models.sha256[i]);
 }
 assert(i2s_models_valid(&models));auto bad=models;--bad.bytes[0];assert(!i2s_recognition_start(&bad));
 assert(i2s_recognition_start(&models)&&entry);assert(!i2s_recognition_start(&models));
 uint8_t copied[4096];i2s_recognition_snapshot snapshot{};
 assert(i2s_recognition_copy(0,0,copied,&snapshot)==0);
 if(mode==1)*reinterpret_cast<uint8_t *>(I2S_MODEL_BASE)^=1;
 try{entry(nullptr);}catch(const Done &){}
 if(mode){assert(i2s_recognition_copy(0,0,copied,&snapshot)==-1);assert(calls==(mode==1?0:1));}
 else{
  assert(calls==1&&i2s_recognition_copy(0,0,copied,&snapshot)==1&&snapshot.samples==80000);
  constexpr char expected[]="TRANSCRIPT finite utterance.\n";
  unsigned bytes=snapshot.bytes;assert(bytes==sizeof(expected)-1&&i2s_recognition_copy(0,bytes,copied,&snapshot)==1);
  assert(!std::memcmp(copied,expected,bytes));
  uint8_t hash[32];storage_digest_bytes(copied,bytes,hash);assert(!std::memcmp(hash,snapshot.text_sha256,32));
  assert(i2s_recognition_copy(bytes,1,copied,&snapshot)==-1);
  assert(i2s_recognition_copy(0,1,retained.text,&snapshot)==-1);
  healthy=false;assert(i2s_recognition_copy(0,0,copied,&snapshot)==-1);
 }
 munmap(mapped,2*I2S_MODEL_SLOT);puts("PASS I2S WAV/model handoff and private transcription publication");
}
