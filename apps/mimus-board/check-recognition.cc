// Execute unchanged recorded recognition with modeled scheduling/liveness.
#include "application.h"
#include "FreeRTOS.h"
#include "task.h"
#include "mimus/asr/platform.h"
#include <cassert>
#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>
#include <sys/mman.h>
static TaskFunction_t worker;
static std::jmp_buf parked;
static bool failed,live=true,overlap,cancel;
namespace mimus::asr::platform {
uint64_t MonotonicNanos(){return 1000;}
uintptr_t TaskIdentity(){return 1;}
}
extern "C" TaskHandle_t xTaskGetCurrentTaskHandle(){return reinterpret_cast<TaskHandle_t>(1);}
extern "C" BaseType_t xTaskGetSchedulerState(){return taskSCHEDULER_NOT_STARTED;}
extern "C" TaskHandle_t xTaskCreateStatic(TaskFunction_t f,const char *const,configSTACK_DEPTH_TYPE n,void *const p,UBaseType_t pri,StackType_t *,StaticTask_t *)
{assert(n==16384&&!p&&pri==3);worker=f;return reinterpret_cast<TaskHandle_t>(1);}
extern "C" void vTaskSuspend(TaskHandle_t){std::longjmp(parked,1);}
extern "C" void vTaskDelay(TickType_t){std::longjmp(parked,1);}
extern "C" void fixture_task_yield(){if(mimus_board_started()&&!overlap){mimus_board_storage_completed();overlap=true;if(cancel)live=false;}}
extern "C" void runtime_smp_init(){}
extern "C" void runtime_init(){}
extern "C" size_t runtime_heap_peak(){return 0;}
extern "C" int mimus_board_trigger_step(){return live?1:-1;}
extern "C" int mimus_board_live(){return live;}
extern "C" int mimus_board_session_live(){return live;}
extern "C" void mimus_board_fail(){failed=true;live=false;}
int main(int argc,char **argv)
{
    assert(argc==6);cancel=!std::strcmp(argv[1],"cancel");bool corrupt=!std::strcmp(argv[1],"corrupt");
    const uintptr_t addresses[]={0x44000000,0x44800000,0x45000000};
    for(unsigned i=0;i<3;++i){
        std::ifstream f(argv[i+2],std::ios::binary);assert(f);std::vector<char> data{std::istreambuf_iterator<char>(f),{}};
        size_t extent=(data.size()+4095)&~size_t(4095);
        assert(mmap(reinterpret_cast<void *>(addresses[i]),extent,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0)!=MAP_FAILED);
        std::memcpy(reinterpret_cast<void *>(addresses[i]),data.data(),data.size());
    }
    assert(mmap(reinterpret_cast<void *>(0x46000000),16*1024*1024,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0)!=MAP_FAILED);
    if(corrupt)*reinterpret_cast<uint8_t *>(addresses[0])^=1;
    assert(mimus_board_start()==1&&worker);
    uint8_t out[1024];size_t copied;uint32_t total;
    assert(mimus_board_copy(0,0,out,&copied,&total)==0);
    if(!setjmp(parked))worker(nullptr);
    if(corrupt){assert(failed&&mimus_board_copy(0,0,out,&copied,&total)==-1);std::puts("PASS substituted model rejected before recognition");return 0;}
    if(cancel){assert(failed&&overlap&&mimus_board_copy(0,0,out,&copied,&total)==-1);std::puts("PASS expired recognition parks at existing yield checkpoint");return 0;}
    assert(!failed&&overlap&&mimus_board_copy(0,0,out,&copied,&total)==1&&total);
    FILE *f=std::fopen(argv[5],"wb");assert(f);
    for(uint32_t offset=0;offset<total;){size_t n=total-offset;if(n>1024)n=1024;uint32_t count;
        assert(mimus_board_copy(offset,n,out,&copied,&count)==1&&copied==n&&count==total);
        assert(std::fwrite(out,1,n,f)==n);offset+=n;
    }
    assert(!std::fclose(f));live=false;assert(mimus_board_copy(0,0,out,&copied,&total)==-1);
}
