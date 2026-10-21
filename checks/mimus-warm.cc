// Actual recorded engine retention, load reporting, PCM-to-formatted timing and
// yield quantum on the pinned bench bundle. Clock, scheduler and output sink
// are modeled; arithmetic is the host portable library.
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
static uint64_t clock_ns, reads, yields;
// Each clock read advances 50 us, so one yield needs at least 20 reads.
namespace mimus::asr::platform {
uint64_t MonotonicNanos(){++reads;return clock_ns+=50000;}
uintptr_t TaskIdentity(){return 1;}
}
extern "C" void fixture_task_yield(){++yields;}
extern "C" void platform_puts(const char *text){out+=text;}
extern "C" size_t runtime_heap_peak(){return 0;}
static unsigned count(const std::string &text,const char *needle)
{unsigned n=0;for(size_t at=text.find(needle);at!=std::string::npos;at=text.find(needle,at+1))++n;return n;}
static unsigned long long field(const std::string &text,const char *name)
{size_t at=text.find(name);assert(at!=std::string::npos);return std::stoull(text.substr(at+std::strlen(name)));}
// Output that must not depend on load, timing or scheduling.
static std::string stable(const std::string &text)
{
    std::string kept;size_t begin=0;
    while(begin<text.size()){
        size_t end=text.find('\n',begin);std::string line=text.substr(begin,end-begin+1);begin=end+1;
        for(const char *kind:{"RESULT feat_crc","TEXT ","MARKS ","CAPS ","FORMATTED ","PASS "})
            if(!line.compare(0,std::strlen(kind),kind)){
                if(line[0]=='R')line=line.substr(0,line.find(" compute_us="))+"\n";
                kept+=line;
            }
    }
    return kept;
}
static std::string run(std::span<const uint8_t> inputs[3],unsigned options,bool expect=true,
                       const std::atomic<bool> *stop=nullptr)
{
    static uint8_t scratch[97280];
    out.clear();const uint64_t before=yields,start=reads;
    assert(mimus_transcribe_recorded(inputs,scratch,stop,options)==expect);
    if(expect){
        assert(field(out,"TIMING total_us=")>=field(out,"compute_us="));
        assert(field(out," yields=")==yields-before&&yields-before>1&&(yields-before-1)*20<=reads-start);
    }
    return out;
}
int main(int argc,char **argv)
{
    assert(argc==3);
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
    const unsigned warm=kMimusWarm|kMimusVerified;
    // Two warm requests: one load without the tensor CRC pass, identical results.
    const std::string first=run(inputs,warm),second=run(inputs,warm);
    assert(count(first,"LOAD load_us=")==1&&count(first,"verify_crc=0")==1&&!count(second,"LOAD "));
    assert(stable(first)==stable(second)&&count(stable(first),"\n")==6);
    for(unsigned i=0;i<2;++i){
        std::FILE *log=std::fopen((std::string(argv[2])+(i?"-second.log":"-first.log")).c_str(),"wb");
        const std::string &text=i?second:first;
        assert(log&&std::fwrite(text.data(),1,text.size(),log)==text.size()&&!std::fclose(log));
    }
    // A slot change invalidates the retained engine before its next use.
    mimus_engine_invalidate();
    assert(count(run(inputs,warm),"LOAD ")==1&&!count(run(inputs,warm),"LOAD "));
    // Other model spans never reuse an engine bound to the previous spans.
    std::vector<uint8_t> copy(bundle);
    std::span<const uint8_t> moved[3]={{copy.data()+(inputs[0].data()-bundle.data()),inputs[0].size()},
        {copy.data()+(inputs[1].data()-bundle.data()),inputs[1].size()},inputs[2]};
    assert(count(run(moved,warm),"LOAD ")==1&&stable(run(moved,warm))==stable(first));
    // A failed warm request keeps its engine until the next request reloads it.
    std::vector<uint8_t> broken(inputs[2].begin(),inputs[2].end());broken[0]^=1;
    std::span<const uint8_t> invalid[3]={moved[0],moved[1],broken};
    assert(count(run(invalid,warm,false),"ERROR ")==1);
    assert(count(run(moved,warm),"LOAD ")==1);
    // A cancelled request loads nothing.
    std::atomic<bool> stop{true};
    assert(run(moved,warm,false,&stop).empty());
    // Cold requests keep the original per-request load with tensor CRCs.
    const std::string cold=run(inputs,0);
    assert(count(cold,"LOAD ")==1&&count(cold,"verify_crc=1")==1&&stable(cold)==stable(first));
    assert(count(run(inputs,0),"LOAD ")==1&&count(run(inputs,warm),"LOAD ")==1);
    std::puts("PASS warm engine loads once per model generation with unchanged results; cold requests reload");
}
