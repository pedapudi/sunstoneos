#include "recognition.h"
extern "C" {
#include "headset.h"
#include "pcm.h"
#include "a733_storage_digest.h"
}
#include "../mimus/recorded.h"
#include "runtime.h"
#include "mimus/asr/platform.h"
#include "FreeRTOS.h"
#include "task.h"
#include <atomic>
#include <cstring>
#include <span>
extern "C" void runtime_smp_init(void);
namespace {
struct Retained {
    bluetooth_application *app;
    bluetooth_recognition_profile models;
    uint64_t deadline;
    TaskHandle_t owner;
    StaticTask_t task;
    StackType_t stack[16384];
    alignas(64) uint8_t scratch[97280];
    alignas(4) uint8_t wav[BLUETOOTH_WAV_BYTES];
    uint8_t pcm[48000],log[BLUETOOTH_RECOGNITION_LOG];
    bluetooth_recognition_snapshot snapshot;
    unsigned used;
} retained;
std::atomic<unsigned> terminal{0};
std::atomic<bool> stopped{false};
uint64_t now(){return mimus::asr::platform::MonotonicNanos()/1000;}

bool live()
{return retained.app&&!stopped.load(std::memory_order_acquire)&&now()<retained.deadline&&bluetooth_recognition_live(retained.app)==1;}
void fail()
{stopped.store(true,std::memory_order_release);terminal.store(2,std::memory_order_release);}
[[noreturn]] void failed(){fail();for(;;)vTaskSuspend(nullptr);}
bool separate(const void *a,size_t n,const void *b,size_t m)
{uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;return a&&b&&n<=UINTPTR_MAX-x&&m<=UINTPTR_MAX-y&&(x+n<=y||y+m<=x);}
bool model_hash(unsigned i)
{
    const uint8_t *data=reinterpret_cast<const uint8_t *>(BLUETOOTH_MODEL_BASE+i*BLUETOOTH_MODEL_SLOT);
    storage_digest digest;storage_digest_init(&digest);
    for(uint64_t offset=0;offset<retained.models.bytes[i];){
        if(!live())return false;
        size_t count=retained.models.bytes[i]-offset;if(count>4096)count=4096;
        if(!storage_digest_add(&digest,data+offset,count))return false;offset+=count;
    }
    uint8_t hash[32];storage_digest_end(&digest,hash);
    return live()&&!std::memcmp(hash,retained.models.sha256[i],32);
}
void recognize(void *)
{
    bluetooth_pcm_snapshot capture{};size_t copied=0;uint8_t chunk[1024];int result=0;
    while(live()){
        result=bluetooth_headset_copy(retained.app,0,0,2,chunk,&copied,&capture);
        if(result<0){failed();}
        if(result==1)break;
        vTaskDelay(1);
    }
    if(result!=1||!live()||!capture.bytes||(capture.bytes&1)||capture.bytes>sizeof(retained.pcm)||capture.loss_packets){failed();}
    const bluetooth_pcm_snapshot identity=capture;
    for(uint32_t offset=0;offset<identity.bytes;){
        size_t count=identity.bytes-offset;if(count>1024)count=1024;
        if(!live()||bluetooth_headset_copy(retained.app,0,offset,count,chunk,&copied,&capture)!=1||copied!=count||
            std::memcmp(&capture,&identity,sizeof(identity))){failed();}
        std::memcpy(retained.pcm+offset,chunk,count);offset+=static_cast<uint32_t>(count);
    }
    uint8_t hash[32];storage_digest_bytes(retained.pcm,identity.bytes,hash);
    if(!live()||std::memcmp(hash,identity.sha256,32)){failed();}
    size_t wav_bytes=0;
    if(bluetooth_pcm_wav(retained.pcm,identity.bytes,retained.wav,sizeof(retained.wav),&wav_bytes)!=1||!live()||!model_hash(0)||!model_hash(1)){
        failed();
    }
    std::span<const uint8_t> inputs[3]={
        {reinterpret_cast<const uint8_t *>(BLUETOOTH_MODEL_BASE),static_cast<size_t>(retained.models.bytes[0])},
        {reinterpret_cast<const uint8_t *>(BLUETOOTH_MODEL_BASE+BLUETOOTH_MODEL_SLOT),static_cast<size_t>(retained.models.bytes[1])},
        {retained.wav,wav_bytes}};
    // The shared recorded engine destroys all temporary objects before return.
    // Loading remains finite; revocation denies publication even before its
    // next existing recognition cancellation checkpoint can run.
    bool success=live()&&mimus_transcribe_recorded(inputs,retained.scratch,&stopped);
    if(!success||!live()){failed();}
    retained.snapshot.firmware=identity.firmware;retained.snapshot.uart=identity.uart;
    retained.snapshot.acl=identity.acl;retained.snapshot.audio=identity.audio;
    retained.snapshot.bytes=retained.used;retained.snapshot.input_samples=identity.bytes/2;
    retained.snapshot.output_samples=identity.bytes;
    std::memcpy(retained.snapshot.pcm_sha256,identity.sha256,32);
    storage_digest_bytes(retained.wav,wav_bytes,retained.snapshot.wav_sha256);
    storage_digest_bytes(retained.log,retained.used,retained.snapshot.text_sha256);
    if(!live()){failed();}
    terminal.store(1,std::memory_order_release);for(;;)vTaskSuspend(nullptr);
}
}
extern "C" void platform_puts(const char *text)
{
    if(xTaskGetCurrentTaskHandle()!=retained.owner||!live()){fail();return;}
    size_t length=0;size_t available=sizeof(retained.log)-retained.used;
    while(length<available&&text[length])++length;
    if(length==available){fail();return;}
    std::memcpy(retained.log+retained.used,text,length);retained.used+=static_cast<unsigned>(length);
}
extern "C" void platform_hex(uint64_t value)
{static const char hex[]="0123456789abcdef";char text[17];for(unsigned i=0;i<16;++i)text[i]=hex[(value>>(60-4*i))&15];text[16]=0;platform_puts(text);}
extern "C" int bluetooth_recognition_start(bluetooth_application *app,const bluetooth_recognition_profile *models,uint64_t deadline)
{
    if(!app||!models||retained.app||!deadline||xTaskGetSchedulerState()!=taskSCHEDULER_NOT_STARTED)return 0;
    for(unsigned i=0;i<2;++i)if(!models->bytes[i]||models->bytes[i]>BLUETOOTH_MODEL_SLOT)return 0;
    retained.app=app;retained.models=*models;retained.deadline=deadline;
    runtime_smp_init();runtime_init();
    retained.owner=xTaskCreateStatic(recognize,"headset-mimus",16384,nullptr,3,retained.stack,&retained.task);
    return retained.owner!=nullptr;
}
extern "C" void bluetooth_recognition_abort(void){fail();}
extern "C" int bluetooth_recognition_copy(bluetooth_application *app,uint32_t offset,size_t bytes,uint8_t out[1024],
                                           size_t *copied,bluetooth_recognition_snapshot *snapshot)
{
    if(!app||app!=retained.app||!bytes||bytes>1024||!separate(out,1024,copied,sizeof(*copied))||
        !separate(out,1024,snapshot,sizeof(*snapshot))||!separate(copied,sizeof(*copied),snapshot,sizeof(*snapshot)))return -1;
    const void *buffers[]={out,copied,snapshot};size_t sizes[]={1024,sizeof(*copied),sizeof(*snapshot)};
    for(unsigned i=0;i<3;++i)if(bluetooth_recognition_output(app,buffers[i],sizes[i])!=1||!separate(buffers[i],sizes[i],&retained,sizeof(retained))||
        !separate(buffers[i],sizes[i],reinterpret_cast<const void *>(BLUETOOTH_MODEL_BASE),2*BLUETOOTH_MODEL_SLOT)||
        !separate(buffers[i],sizes[i],reinterpret_cast<const void *>(0x46000000),16*1024*1024))return -1;
    *copied=0;std::memset(snapshot,0,sizeof(*snapshot));
    unsigned state=terminal.load(std::memory_order_acquire);
    if(!live()||state==2)return -1;if(!state)return 0;
    if(offset>retained.used||bytes>retained.used-offset)return -1;
    std::memcpy(out,retained.log+offset,bytes);*copied=bytes;*snapshot=retained.snapshot;
    return live()?1:-1;
}
