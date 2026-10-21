// Warm engine reuse with mixed load options on the pinned bench bundle: an
// engine loaded without the tensor CRC pass serves only kMimusVerified callers.
// Clock, scheduler and output sink are modeled as in checks/mimus-warm.cc.
#include "../apps/mimus/recorded.h"
#include "FreeRTOS.h"
#include "mimus/asr/platform.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include <sys/mman.h>
static std::string out;
static uint64_t clock_ns;
namespace mimus::asr::platform {
uint64_t MonotonicNanos(){return clock_ns+=50000;}
uintptr_t TaskIdentity(){return 1;}
}
extern "C" void fixture_task_yield(){}
extern "C" void platform_puts(const char *text){out+=text;}
extern "C" size_t runtime_heap_peak(){return 0;}
static unsigned count(const std::string &text,const char *needle)
{unsigned n=0;for(size_t at=text.find(needle);at!=std::string::npos;at=text.find(needle,at+1))++n;return n;}
static std::string text(const std::string &log)
{size_t at=log.find("\nTEXT ");assert(at!=std::string::npos);return log.substr(at,log.find('\n',at+1)-at);}
static std::string run(std::span<const uint8_t> inputs[3],unsigned options)
{
    static uint8_t scratch[97280];
    out.clear();assert(mimus_transcribe_recorded(inputs,scratch,nullptr,options));return out;
}
int main(int argc,char **argv)
{
    assert(argc==2);
    std::ifstream file(argv[1],std::ios::binary);assert(file);
    std::vector<uint8_t> bundle{std::istreambuf_iterator<char>(file),{}};
    assert(bundle.size()>128&&!std::memcmp(bundle.data(),"SUNMIM01",8));
    uint64_t sizes[3];std::memcpy(sizes,bundle.data()+8,sizeof(sizes));
    std::span<const uint8_t> inputs[3];size_t offset=128;
    for(unsigned i=0;i<3;++i){
        assert(sizes[i]&&sizes[i]<=bundle.size()-offset);
        inputs[i]={bundle.data()+offset,size_t(sizes[i])};offset=(offset+sizes[i]+63)&~size_t(63);
    }
    assert(mmap(reinterpret_cast<void *>(kMimusArenaBase),kMimusArenaBytes,PROT_READ|PROT_WRITE,
                MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0)==reinterpret_cast<void *>(kMimusArenaBase));
    const unsigned verified=kMimusWarm|kMimusVerified;
    const std::string unchecked=run(inputs,verified);
    assert(count(unchecked,"LOAD ")==1&&count(unchecked,"verify_crc=0")==1);
    // A caller without kMimusVerified reloads with CRCs instead of reusing it.
    const std::string checked=run(inputs,kMimusWarm);
    assert(count(checked,"LOAD ")==1&&count(checked,"verify_crc=1")==1);
    // The CRC-checked engine then serves both kinds of caller.
    const std::string again=run(inputs,verified),plain=run(inputs,kMimusWarm);
    assert(!count(again,"LOAD ")&&!count(plain,"LOAD "));
    assert(text(unchecked)==text(checked)&&text(checked)==text(again)&&text(again)==text(plain));
    mimus_engine_release();
    std::puts("PASS warm engine loaded without tensor CRCs is reloaded for a caller that needs them");
}
