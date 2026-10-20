#include "application.h"
#include "assets.h"
#include "../mimus/recorded.h"
#include "runtime.h"
extern "C" {
#include "a733_storage_digest.h"
#include "FreeRTOS.h"
#include "task.h"
void runtime_smp_init(void);
}
#include <atomic>
#include <cstring>
namespace {
StaticTask_t task;
StackType_t stack[16384];
alignas(64) uint8_t scratch[97280];
uint8_t transcript[16384];
unsigned used;
std::atomic<unsigned> state{0},started{0},overlap{0},generation{1},acknowledged{0};
std::atomic<bool> stopped{false};
void fail(){stopped.store(true,std::memory_order_release);state.store(2,std::memory_order_release);mimus_board_fail();}
[[noreturn]] void failed(){fail();for(;;)vTaskSuspend(nullptr);}
void recognize(void *)
{
    /* Board owners must first establish live storage attachment. Hashing is
     * EL0 CPU work; publication in EL3 alone does not authenticate inputs. */
    while(!mimus_board_live()){if(stopped.load()||!mimus_board_session_live()){failed();}vTaskDelay(1);}
    for(unsigned i=0;i<3;++i){
        storage_digest digest;storage_digest_init(&digest);
        const auto *p=reinterpret_cast<const uint8_t *>(mimus_asset_address[i]);
        for(size_t n=0;n<mimus_asset_bytes[i];){
            if(!mimus_board_live()){failed();}size_t count=mimus_asset_bytes[i]-n;if(count>4096)count=4096;
            if(!storage_digest_add(&digest,p+n,count)){failed();}n+=count;taskYIELD();
        }
        uint8_t hash[32];storage_digest_end(&digest,hash);
        if(std::memcmp(hash,mimus_asset_sha256[i],32)){failed();}
    }
    std::span<const uint8_t> input[3];for(unsigned i=0;i<3;++i)
        input[i]={reinterpret_cast<const uint8_t *>(mimus_asset_address[i]),mimus_asset_bytes[i]};
    /* Only this task mutates recognizer/transcript storage. USB0 publishes an
     * ACK only after all result bytes and the ACK response were delivered. */
    for(;;){
        for(;;){
            int trigger=mimus_board_trigger_step();if(trigger<0){failed();}
            if(trigger==1)break;vTaskDelay(1);
        }
        unsigned current=generation.load(std::memory_order_acquire);
        started.store(current,std::memory_order_release);
        bool ok=mimus_transcribe_recorded(input,scratch,&stopped);
        started.store(0,std::memory_order_release);
        if(!ok||!mimus_board_live()||state.load(std::memory_order_acquire)==2){failed();}
        /* A read finishing after recognition is not overlap. */
        if(overlap.load(std::memory_order_acquire)!=current){failed();}
        state.store(1,std::memory_order_release);
        if(!mimus_board_keyboard())for(;;)vTaskDelay(1);
        while(acknowledged.load(std::memory_order_acquire)!=current){
            if(!mimus_board_live()){failed();}vTaskDelay(1);
        }
        if(current==UINT32_MAX){failed();} /* Refuse before identity wraps. */
        state.store(0,std::memory_order_release);
        used=0;overlap.store(0,std::memory_order_release);
        generation.store(current+1,std::memory_order_release);
        if(!mimus_board_rearm()){failed();}
    }
}
}
extern "C" void mimus_native_checkpoint(void)
{if(!mimus_board_live())failed();}
extern "C" void platform_puts(const char *text)
{
    mimus_native_checkpoint();
    if(stopped.load(std::memory_order_acquire))return;
    size_t n=0;while(text[n]&&n<sizeof(transcript)-used)++n;
    if(n==sizeof(transcript)-used){failed();}
    std::memcpy(transcript+used,text,n);used+=n;
}
extern "C" void platform_hex(uint64_t x)
{char b[17];static const char h[]="0123456789abcdef";for(unsigned i=0;i<16;++i)b[i]=h[(x>>(60-4*i))&15];b[16]=0;platform_puts(b);}
extern "C" int mimus_board_start(void)
{runtime_smp_init();runtime_init();return xTaskCreateStatic(recognize,"mimus-recorded",16384,nullptr,3,stack,&task)!=nullptr;}
extern "C" unsigned mimus_board_started(void){return started.load(std::memory_order_acquire);}
extern "C" unsigned mimus_board_generation(void){return generation.load(std::memory_order_acquire);}
extern "C" int mimus_board_ack(unsigned current)
{
    if(!mimus_board_keyboard()||!mimus_board_live()||state.load(std::memory_order_acquire)!=1||
       current!=generation.load(std::memory_order_acquire)||acknowledged.load(std::memory_order_acquire)==current)return 0;
    /* USB0 is the sole result reader and ACK producer. Close its admission
     * synchronously before the worker may reuse any published bytes. */
    state.store(3,std::memory_order_release);
    acknowledged.store(current,std::memory_order_release);return 1;
}
extern "C" void mimus_board_storage_completed(unsigned current)
{if(current&&started.load(std::memory_order_acquire)==current)overlap.store(current,std::memory_order_release);}
extern "C" int mimus_board_copy(uint32_t offset,size_t bytes,uint8_t *out,size_t *copied,uint32_t *total)
{
    *copied=0;*total=0;
    if(!mimus_board_session_live()||state.load(std::memory_order_acquire)==2)return -1;
    if(state.load(std::memory_order_acquire)!=1)return 0;
    if(!mimus_board_live()||overlap.load(std::memory_order_acquire)!=generation.load(std::memory_order_acquire))return -1;
    *total=used;if(offset>used||bytes>used-offset||bytes>1024)return -1;
    std::memcpy(out,transcript+offset,bytes);*copied=bytes;return mimus_board_live()?1:-1;
}
