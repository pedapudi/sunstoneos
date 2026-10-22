// Actual finite converter/recognition bridge and shared recorded engine.
// Scheduler, current session and post-disconnect copy publication are modeled.
#include "../apps/bluetooth/recognition.h"
extern "C" {
#include "../apps/bluetooth/headset.h"
#include "a733_storage_digest.h"
}
#include "FreeRTOS.h"
#include "task.h"
#include "mimus/asr/platform.h"
#include <cassert>
#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <vector>
#include <sys/mman.h>
static std::jmp_buf parked;
static TaskFunction_t worker;
static TaskHandle_t task=reinterpret_cast<TaskHandle_t>(1);
static bluetooth_application *app=reinterpret_cast<bluetooth_application *>(0x10000);
static bool scheduler_started,revoked,loss,failed_capture,cancel,stopped_in_engine;
static unsigned delays,requests;
static uint64_t clock_us=1;
static std::vector<uint8_t> pcm;
static bluetooth_pcm_snapshot identity;
namespace mimus::asr::platform {
uint64_t MonotonicNanos(){return clock_us*1000;}
uintptr_t TaskIdentity(){return 1;}
}
extern "C" TaskHandle_t xTaskGetCurrentTaskHandle(){return task;}
extern "C" BaseType_t xTaskGetSchedulerState(){return scheduler_started?taskSCHEDULER_RUNNING:taskSCHEDULER_NOT_STARTED;}
extern "C" TaskHandle_t xTaskCreateStatic(TaskFunction_t fn,const char *const name,const configSTACK_DEPTH_TYPE depth,
                                         void *const context,UBaseType_t priority,StackType_t *const stack,StaticTask_t *const storage)
{assert(fn&&name&&depth==16384&&!context&&priority==3&&stack&&storage);worker=fn;return task;}
extern "C" void vTaskDelay(TickType_t){++delays;}
extern "C" void vTaskSuspend(TaskHandle_t){std::longjmp(parked,1);}
extern "C" void fixture_task_yield(){if(cancel&&!stopped_in_engine){stopped_in_engine=true;revoked=true;bluetooth_recognition_abort();}}
extern "C" void vTaskYieldWithinAPI(){ }
extern "C" void runtime_smp_init(){assert(!scheduler_started);}
extern "C" void runtime_init(){assert(!scheduler_started);}
extern "C" size_t runtime_heap_peak(){return 0;}
extern "C" int bluetooth_recognition_live(bluetooth_application *p){assert(p==app);return !revoked;}
extern "C" int bluetooth_recognition_output(bluetooth_application *p,const void *out,size_t bytes){assert(p==app&&out&&bytes);return 1;}
extern "C" int bluetooth_headset_copy(bluetooth_application *p,unsigned kind,uint32_t offset,size_t bytes,uint8_t out[1024],
                                      size_t *copied,bluetooth_pcm_snapshot *snapshot)
{
    assert(p==app&&!kind&&bytes<=1024&&offset+bytes<=pcm.size());++requests;
    *copied=0;std::memset(snapshot,0,sizeof(*snapshot));
    if(failed_capture)return -1;
    if(delays<2)return 0;
    *snapshot=identity;if(loss)snapshot->loss_packets=1;
    std::memcpy(out,pcm.data()+offset,bytes);*copied=bytes;return 1;
}
// Finite fixture selects mode1; repeated-only entry points are unreachable.
extern "C" int bluetooth_headset_wideband(const bluetooth_application *){return 0;}
extern "C" int bluetooth_headset_repeated(const bluetooth_application *){return 0;}
extern "C" int bluetooth_headset_segment_bind(bluetooth_application *,void *){assert(false);return -1;}
extern "C" int bluetooth_headset_segment_take(bluetooth_application *,const uint8_t **,bluetooth_segment_snapshot *){assert(false);return -1;}
extern "C" int bluetooth_headset_segment_request(bluetooth_application *,unsigned,uint64_t){assert(false);return -1;}
extern "C" uint32_t ulTaskNotifyTake(BaseType_t,TickType_t){assert(false);return 0;}
// The failure path wakes the recognizer, which waits only when repeated.
extern "C" BaseType_t xTaskNotifyGive(TaskHandle_t t){assert(t==task);return 1;}
extern "C" int bluetooth_headset_segment_ack(bluetooth_application *,uint64_t){assert(false);return -1;}
extern "C" int bluetooth_headset_segment_status(bluetooth_application *,bluetooth_segment_status *){assert(false);return -1;}
extern "C" int bluetooth_headset_segment_stop(bluetooth_application *){assert(false);return -1;}
static std::vector<uint8_t> read(const char *path)
{std::ifstream f(path,std::ios::binary);assert(f);return {std::istreambuf_iterator<char>(f),{}};}
int main(int argc,char **argv)
{
    assert(argc==6);pcm=read(argv[2]);assert(!pcm.empty()&&!(pcm.size()&1)&&pcm.size()<=48000);
    cancel=!std::strcmp(argv[1],"cancel");loss=!std::strcmp(argv[1],"loss");failed_capture=!std::strcmp(argv[1],"failed-capture");
    identity={4,9,1,1,1,static_cast<uint32_t>(pcm.size()),1,0,{0}};
    storage_digest_bytes(pcm.data(),pcm.size(),identity.sha256);
    bluetooth_recognition_profile models{};
    for(unsigned i=0;i<2;++i){auto file=read(argv[3+i]);models.bytes[i]=file.size();assert(file.size()<=BLUETOOTH_MODEL_SLOT);
        void *memory=mmap(reinterpret_cast<void *>(BLUETOOTH_MODEL_BASE+i*BLUETOOTH_MODEL_SLOT),BLUETOOTH_MODEL_SLOT,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);assert(memory!=MAP_FAILED);
        std::memcpy(memory,file.data(),file.size());storage_digest_bytes(file.data(),file.size(),models.sha256[i]);}
    assert(mmap(reinterpret_cast<void *>(0x46000000),16*1024*1024,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0)!=MAP_FAILED);
    if(!std::strcmp(argv[1],"model-hash"))models.sha256[0][0]^=1;
    uint64_t deadline=!std::strcmp(argv[1],"deadline")?1:60000000;
    assert(bluetooth_recognition_start(app,&models,deadline)==1&&worker);
    uint8_t copied[1024];size_t count=99;bluetooth_recognition_snapshot snapshot;
    std::memset(&snapshot,0xa5,sizeof(snapshot));const bluetooth_recognition_snapshot empty{};
    if(deadline>clock_us)assert(bluetooth_recognition_copy(app,0,1,copied,&count,&snapshot)==0&&!count&&!std::memcmp(&snapshot,&empty,sizeof(empty)));
    scheduler_started=true;
    if(!setjmp(parked))worker(nullptr);
    bool negative=loss||failed_capture||cancel||!std::strcmp(argv[1],"model-hash")||!std::strcmp(argv[1],"deadline");
    if(negative){if(cancel)assert(stopped_in_engine);assert(bluetooth_recognition_copy(app,0,1,copied,&count,&snapshot)==-1&&!count&&!std::memcmp(&snapshot,&empty,sizeof(empty)));
        std::printf("PASS retained recognition refusal: %s\n",argv[1]);return 0;}
    assert(bluetooth_recognition_copy(app,0,1,copied,&count,&snapshot)==1&&count==1);
    assert(snapshot.input_samples==pcm.size()/2&&snapshot.output_samples==pcm.size()&&snapshot.bytes<=65536);
    std::vector<uint8_t> log;
    for(uint32_t offset=0;offset<snapshot.bytes;){size_t bytes=snapshot.bytes-offset;if(bytes>1024)bytes=1024;
        bluetooth_recognition_snapshot next;assert(bluetooth_recognition_copy(app,offset,bytes,copied,&count,&next)==1&&count==bytes&&!std::memcmp(&snapshot,&next,sizeof(next)));
        log.insert(log.end(),copied,copied+bytes);offset+=static_cast<uint32_t>(bytes);}
    uint8_t hash[32];storage_digest_bytes(log.data(),log.size(),hash);assert(!std::memcmp(hash,snapshot.text_sha256,32));
    FILE *file=std::fopen(argv[5],"wb");assert(file&&std::fwrite(log.data(),1,log.size(),file)==log.size()&&!std::fclose(file));
    std::string metadata=std::string(argv[5])+".snapshot.bin";file=std::fopen(metadata.c_str(),"wb");
    assert(file&&std::fwrite(&snapshot,1,sizeof(snapshot),file)==sizeof(snapshot)&&!std::fclose(file));
    revoked=true;assert(bluetooth_recognition_copy(app,0,1,copied,&count,&snapshot)==-1&&!count&&!std::memcmp(&snapshot,&empty,sizeof(empty)));
    std::puts("PASS finite capture copy→8to16k→unchanged model/formatter→copied text and retained revocation");
}
