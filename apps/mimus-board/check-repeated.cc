/* Actual recognition-task lifetime with modeled recognizer and scheduling.
 * No arithmetic, devices, boot or cache effects execute. */
#include "application.h"
#include "keyboard.h"
#include "../mimus/recorded.h"
#include "FreeRTOS.h"
#include "task.h"
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <mutex>
#include <span>
#include <thread>
#include <vector>
#include <sys/mman.h>
extern "C" void platform_puts(const char *);
static TaskFunction_t worker;
static std::mutex gate;
static std::condition_variable changed;
static bool paused,advance;
static std::atomic<bool> live{true},failed{false};
static unsigned calls;
static bool no_overlap;
struct TaskStopped {};
extern "C" TaskHandle_t xTaskCreateStatic(TaskFunction_t f,const char *const,configSTACK_DEPTH_TYPE,void *const,UBaseType_t,StackType_t *,StaticTask_t *)
{worker=f;return reinterpret_cast<TaskHandle_t>(1);}
extern "C" void vTaskDelay(TickType_t)
{std::unique_lock lock(gate);paused=true;changed.notify_all();changed.wait(lock,[]{return advance;});advance=false;paused=false;}
extern "C" void vTaskSuspend(TaskHandle_t){throw TaskStopped{};}
extern "C" void fixture_task_yield(){}
extern "C" void runtime_smp_init(){}
extern "C" void runtime_init(){}
extern "C" int mimus_board_live(){return live.load();}
extern "C" int mimus_board_session_live(){return live.load();}
extern "C" int mimus_board_keyboard(){return 1;}
extern "C" int mimus_board_trigger_step(){return live.load()?mimus_keyboard_step():-1;}
extern "C" int mimus_board_rearm(){return mimus_keyboard_rearm();}
extern "C" void mimus_board_fail(){failed=true;live=false;changed.notify_all();}
bool mimus_transcribe_recorded(std::span<const uint8_t>[3],std::span<uint8_t>,const std::atomic<bool> *)
{
    ++calls;assert(mimus_board_started()==calls);
    platform_puts(calls==1?"first immutable transcript\n":"second immutable transcript\n");
    if(!no_overlap)mimus_board_storage_completed(calls);
    return true;
}
static void wait_pause()
{std::unique_lock lock(gate);changed.wait(lock,[]{return paused||failed.load();});}
static void tick()
{std::unique_lock lock(gate);assert(paused);paused=false;advance=true;changed.notify_all();changed.wait(lock,[]{return paused||failed.load();});}
int main(int argc,char **argv)
{
    assert(argc==5);no_overlap=!std::strcmp(argv[1],"no-overlap");
    uintptr_t addresses[]={0x44000000,0x44800000,0x45000000};
    for(unsigned i=0;i<3;++i){
        std::ifstream f(argv[i+2],std::ios::binary);assert(f);
        std::vector<char> data{std::istreambuf_iterator<char>(f),{}};
        assert(mmap(reinterpret_cast<void *>(addresses[i]),(data.size()+4095)&~size_t(4095),PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0)!=MAP_FAILED);
        std::memcpy(reinterpret_cast<void *>(addresses[i]),data.data(),data.size());
    }
    assert(mimus_board_start());
    std::thread task([]{try{worker(nullptr);}catch(const TaskStopped&){};});wait_pause();
    struct mimus_keyboard_report report={};report.generation=23;report.address=3;report.speed=1;report.protocol=1;report.endpoint=0x81;report.subclass=1;report.bytes=8;
    for(unsigned current=1;current<=2;++current){
        assert(mimus_board_generation()==current);
        // Held Enter cannot inherit the previous trigger's neutral report.
        if(current==2){++report.sequence;report.data[2]=0x28;assert(mimus_keyboard_offer(&report)==1);tick();assert(calls==1);}
        ++report.sequence;report.data[2]=0;assert(mimus_keyboard_offer(&report)==1);tick();
        ++report.sequence;report.data[2]=0x28;assert(mimus_keyboard_offer(&report)==1);tick();
        if(no_overlap){assert(failed&&calls==1);task.join();return 0;}
        assert(calls==current&&!failed&&mimus_board_started()==0);
        uint8_t text[1024],again[1024];size_t copied;uint32_t total;
        assert(mimus_board_copy(0,0,text,&copied,&total)==1&&total);
        assert(mimus_board_copy(0,total,text,&copied,&total)==1&&copied==total);
        assert(!mimus_board_ack(current+1));tick();
        uint32_t repeated;assert(mimus_board_copy(0,total,again,&copied,&repeated)==1&&total==repeated&&!std::memcmp(text,again,total));
        assert(mimus_keyboard_offer(&report)==0);
        assert(mimus_board_ack(current)&&!mimus_board_ack(current));tick();
    }
    assert(mimus_board_generation()==3&&calls==2&&!failed);
    live=false;tick();task.join();assert(failed);
}
