#include "recognition.h"
extern "C" {
#include "headset.h"
#include "pcm.h"
#include "msbc.h"
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
    uint8_t log[BLUETOOTH_RECOGNITION_LOG];
    bluetooth_recognition_snapshot snapshot;
    bluetooth_repeated_snapshot segment;
    unsigned used;
} retained;
// A finite capture is staged in the internal arena, which is unused until
// transcription starts.
static_assert(sizeof(retained.scratch)>=BLUETOOTH_PCM_BYTES);
std::atomic<unsigned> terminal{0};
std::atomic<bool> stopped{false};
std::atomic<unsigned> finished{0};
uint64_t now(){return mimus::asr::platform::MonotonicNanos()/1000;}
// Waits for a segment, a stop or USB collection; every wake-up re-reads state.
constexpr TickType_t kWait=pdMS_TO_TICKS(100);

bool live()
{return retained.app&&!stopped.load(std::memory_order_acquire)&&now()<retained.deadline&&bluetooth_recognition_live(retained.app)==1;}
void fail()
{if(retained.app&&bluetooth_headset_repeated(retained.app)==1)(void)bluetooth_headset_segment_stop(retained.app);
 stopped.store(true,std::memory_order_release);terminal.store(2,std::memory_order_release);
 if(retained.owner)xTaskNotifyGive(retained.owner);}
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
bool valid(const bluetooth_pcm_snapshot &c)
{return live()&&c.state>=1&&c.state<=(bluetooth_headset_wideband(retained.app)?2u:1u)&&c.bytes&&!(c.bytes&1)&&
    c.bytes<=BLUETOOTH_PCM_BYTES&&!c.loss_packets;}
// mSBC PCM is already 16 kHz; CVSD PCM is interpolated.
size_t convert(const uint8_t *pcm,const bluetooth_pcm_snapshot &c)
{
    size_t bytes=0;
    int converted=c.state==2?bluetooth_pcm16_wav(pcm,c.bytes,retained.wav,sizeof(retained.wav),&bytes):
        bluetooth_pcm_wav(pcm,c.bytes,retained.wav,sizeof(retained.wav),&bytes);
    if(converted!=1||!live()){failed();}
    return bytes;
}
// Transcribes the converted WAV and records the result; the PCM digest is
// the producer's.
void transcribe(const bluetooth_pcm_snapshot &identity,size_t wav_bytes,bool repeated)
{
    retained.used=0;
    std::span<const uint8_t> inputs[3]={
        {reinterpret_cast<const uint8_t *>(BLUETOOTH_MODEL_BASE),static_cast<size_t>(retained.models.bytes[0])},
        {reinterpret_cast<const uint8_t *>(BLUETOOTH_MODEL_BASE+BLUETOOTH_MODEL_SLOT),static_cast<size_t>(retained.models.bytes[1])},
        {retained.wav,wav_bytes}};
    // Repeated segments reuse one engine loaded from the authenticated models;
    // a finite capture destroys it before return. Loading remains finite;
    // revocation denies publication even before its next existing recognition
    // cancellation checkpoint can run.
    bool success=live()&&mimus_transcribe_recorded(inputs,retained.scratch,&stopped,
        repeated?kMimusWarm|kMimusVerified:kMimusVerified);
    if(!success||!live()){failed();}
    retained.snapshot.firmware=identity.firmware;retained.snapshot.uart=identity.uart;
    retained.snapshot.acl=identity.acl;retained.snapshot.audio=identity.audio;
    retained.snapshot.bytes=retained.used;retained.snapshot.input_samples=identity.bytes/2;
    retained.snapshot.output_samples=identity.state==2?identity.bytes/2:identity.bytes;
    retained.snapshot.reserved=bluetooth_headset_wideband(retained.app)?identity.state:0;
    std::memcpy(retained.snapshot.pcm_sha256,identity.sha256,32);
    storage_digest_bytes(retained.wav,wav_bytes,retained.snapshot.wav_sha256);
    storage_digest_bytes(retained.log,retained.used,retained.snapshot.text_sha256);
    if(!live()){failed();}
}
// Profile version 3 with mode 1: one finite capture, copied after the
// producer's confirmed disconnect and checked against its digest.
[[noreturn]] void finite()
{
    bluetooth_pcm_snapshot capture{};size_t copied=0;uint8_t chunk[1024];int result=0;
    while(live()){
        result=bluetooth_headset_copy(retained.app,0,0,2,chunk,&copied,&capture);
        if(result<0){failed();}if(result==1)break;
        vTaskDelay(1);
    }
    if(result!=1||!valid(capture)){failed();}
    const bluetooth_pcm_snapshot identity=capture;uint8_t *staged=retained.scratch;
    for(uint32_t offset=0;offset<identity.bytes;){
        size_t count=identity.bytes-offset;if(count>sizeof(chunk))count=sizeof(chunk);
        if(bluetooth_headset_copy(retained.app,0,offset,count,chunk,&copied,&capture)!=1||!live()||copied!=count||
           std::memcmp(&capture,&identity,sizeof(identity))){failed();}
        std::memcpy(staged+offset,chunk,count);offset+=static_cast<uint32_t>(count);
    }
    uint8_t hash[32];storage_digest_bytes(staged,identity.bytes,hash);
    if(!live()||std::memcmp(hash,identity.sha256,32)){failed();}
    transcribe(identity,convert(staged,identity),false);
    terminal.store(1,std::memory_order_release);
    for(;;)vTaskSuspend(nullptr);
}
void recognize(void *)
{
    // USB cannot replace model assets during this lifetime, so one bounded
    // hash before the first capture authenticates every later load and use.
    if(!model_hash(0)||!model_hash(1)){failed();}
    if(bluetooth_headset_repeated(retained.app)!=1)finite();
    // The first request starts an activation at once, so an unattended
    // supervised session records one segment and exercises both +BVRA
    // responses; later activations wait for the headset button.
    unsigned request=BLUETOOTH_SEGMENT_START;
    for(;;){
        bluetooth_segment_snapshot segment{};const uint8_t *pcm=nullptr;int result=0;
        while(live()){
            result=bluetooth_headset_segment_take(retained.app,&pcm,&segment);
            if(result<0){failed();}if(result==1)break;
            bluetooth_segment_status state{};
            if(bluetooth_headset_segment_status(retained.app,&state)!=1){failed();}
            if(state.disconnected&&state.published==state.consumed){
                finished.store(1,std::memory_order_release);for(;;)vTaskSuspend(nullptr);}
            int requested=bluetooth_headset_segment_request(retained.app,request);
            if(requested<0){failed();}if(requested)request=BLUETOOTH_SEGMENT_ACCEPT;
            (void)ulTaskNotifyTake(pdTRUE,kWait);
        }
        if(result!=1||!valid(segment.capture)){failed();}
        // The borrowed slot is released after conversion, so the next
        // segment can record during transcription.
        const size_t wav_bytes=convert(pcm,segment.capture);
        if(bluetooth_headset_segment_ack(retained.app,segment.ticket)!=1){failed();}
        transcribe(segment.capture,wav_bytes,true);
        retained.segment.result=retained.snapshot;retained.segment.ticket=segment.ticket;
        retained.segment.first_sample=segment.first_sample;
        retained.segment.first_acquired=segment.first_acquired;retained.segment.last_acquired=segment.last_acquired;
        terminal.store(1,std::memory_order_release);
        while(live()&&terminal.load(std::memory_order_acquire)==1)(void)ulTaskNotifyTake(pdTRUE,kWait);
        if(!live()||terminal.load(std::memory_order_acquire)==2){failed();}
    }
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
extern "C" int bluetooth_playback_runtime_start(bluetooth_application *app)
{
    if(!app||!bluetooth_headset_wideband(app)||bluetooth_headset_repeated(app)||xTaskGetSchedulerState()!=taskSCHEDULER_NOT_STARTED)return 0;
    runtime_smp_init();runtime_init();
#ifdef SUNSTONE_BLUETOOTH_MSBC
    return bluetooth_msbc_init()==1;
#else
    return 0;
#endif
}
extern "C" int bluetooth_recognition_start(bluetooth_application *app,const bluetooth_recognition_profile *models,uint64_t deadline)
{
    if(!app||!models||retained.app||!deadline||xTaskGetSchedulerState()!=taskSCHEDULER_NOT_STARTED)return 0;
    for(unsigned i=0;i<2;++i)if(!models->bytes[i]||models->bytes[i]>BLUETOOTH_MODEL_SLOT)return 0;
    retained.app=app;retained.models=*models;retained.deadline=deadline;
    runtime_smp_init();runtime_init();
#ifdef SUNSTONE_BLUETOOTH_MSBC
    if(bluetooth_headset_wideband(app)&&bluetooth_msbc_init()!=1)return 0;
#endif
    retained.owner=xTaskCreateStatic(recognize,"headset-mimus",16384,nullptr,3,retained.stack,&retained.task);
    return retained.owner!=nullptr&&(bluetooth_headset_repeated(app)!=1||bluetooth_headset_segment_bind(app,retained.owner)==1);
}
extern "C" void bluetooth_recognition_abort(void){fail();}
extern "C" int bluetooth_recognition_copy(bluetooth_application *app,uint32_t offset,size_t bytes,uint8_t out[1024],
                                           size_t *copied,bluetooth_recognition_snapshot *snapshot)
{
    if(!app||app!=retained.app||bluetooth_headset_repeated(app)==1||!bytes||bytes>1024||!separate(out,1024,copied,sizeof(*copied))||
        !separate(out,1024,snapshot,sizeof(*snapshot))||!separate(copied,sizeof(*copied),snapshot,sizeof(*snapshot)))return -1;
    const void *buffers[]={out,copied,snapshot};size_t sizes[]={1024,sizeof(*copied),sizeof(*snapshot)};
    for(unsigned i=0;i<3;++i)if(bluetooth_recognition_output(app,buffers[i],sizes[i])!=1||!separate(buffers[i],sizes[i],&retained,sizeof(retained))||
        !separate(buffers[i],sizes[i],reinterpret_cast<const void *>(BLUETOOTH_MODEL_BASE),2*BLUETOOTH_MODEL_SLOT)||
        !separate(buffers[i],sizes[i],reinterpret_cast<const void *>(kMimusArenaBase),kMimusArenaBytes))return -1;
    *copied=0;std::memset(snapshot,0,sizeof(*snapshot));
    unsigned state=terminal.load(std::memory_order_acquire);
    if(!live()||state==2)return -1;if(!state)return 0;
    if(offset>retained.used||bytes>retained.used-offset)return -1;
    std::memcpy(out,retained.log+offset,bytes);*copied=bytes;*snapshot=retained.snapshot;
    return live()?1:-1;
}

extern "C" int bluetooth_recognition_segment_copy(bluetooth_application *app,uint64_t ticket,uint32_t offset,size_t bytes,
    uint8_t out[1024],size_t *copied,bluetooth_repeated_snapshot *snapshot)
{
    if(!app||app!=retained.app||bluetooth_headset_repeated(app)!=1||!copied||!snapshot||bytes>1024||
       (bytes&&!out)||!separate(copied,sizeof(*copied),snapshot,sizeof(*snapshot)))return -1;
    const void *buffers[]={out,copied,snapshot};size_t sizes[]={bytes?1024u:0u,sizeof(*copied),sizeof(*snapshot)};
    for(unsigned i=0;i<3;++i)if(sizes[i]&&(bluetooth_recognition_output(app,buffers[i],sizes[i])!=1||
        !separate(buffers[i],sizes[i],&retained,sizeof(retained))||
        !separate(buffers[i],sizes[i],reinterpret_cast<const void *>(BLUETOOTH_MODEL_BASE),2*BLUETOOTH_MODEL_SLOT)||
        !separate(buffers[i],sizes[i],reinterpret_cast<const void *>(kMimusArenaBase),kMimusArenaBytes)))return -1;
    if(bytes&&(!separate(out,1024,copied,sizeof(*copied))||!separate(out,1024,snapshot,sizeof(*snapshot))))return -1;
    *copied=0;std::memset(snapshot,0,sizeof(*snapshot));
    unsigned state=terminal.load(std::memory_order_acquire);
    if(!live()||state==2)return -1;if(!state)return 0;
    if((ticket&&ticket!=retained.segment.ticket)||(bytes&&!ticket)||offset>retained.used||bytes>retained.used-offset)return -1;
    if(bytes)std::memcpy(out,retained.log+offset,bytes);*copied=bytes;*snapshot=retained.segment;return live()?1:-1;
}
extern "C" int bluetooth_recognition_segment_ack(bluetooth_application *app,uint64_t ticket)
{
    if(!app||app!=retained.app||bluetooth_headset_repeated(app)!=1||!ticket||!live()||
       terminal.load(std::memory_order_acquire)!=1||retained.segment.ticket!=ticket)return -1;
    terminal.store(0,std::memory_order_release);xTaskNotifyGive(retained.owner);return 1;
}
extern "C" int bluetooth_recognition_finished(bluetooth_application *app)
{return app==retained.app&&bluetooth_recognition_live(app)==1?static_cast<int>(finished.load(std::memory_order_acquire)):-1;}
