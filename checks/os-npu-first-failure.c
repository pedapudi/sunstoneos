/* Execute the actual OS request predicates. RTOS waits and runtime replies
 * are modeled; no MMIO or accelerator work occurs. */
#include "../apps/os/npu.c"
#include <assert.h>
#include <setjmp.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
struct board_application board;
struct board_provider providers;
static unsigned mode,live_value=1,send_calls,collect_calls,board_failures;
static uint32_t first_site,first_value;
static const void *first_caller;
static uint64_t now_value=1000;
static const uint64_t sent_ticket=UINT64_C(0x123456789abc);
static jmp_buf suspended;
_Noreturn void __assert_func(const char *file,int line,const char *function,const char *condition)
{(void)file;(void)line;(void)function;(void)condition;abort();}
int board_live(void *context){assert(!context);return (int)live_value;}
uint32_t board_live_refusal(void){return live_value?0:UINT32_C(1)<<28;}
void board_fail_note(uint32_t site,uint32_t value,const void *caller)
{if(!first_site){first_site=site;first_value=value;first_caller=caller;}}
void board_fail(void *context){assert(!context);++board_failures;live_value=0;}
void board_fail_at(uint32_t site,uint32_t value,const void *caller)
{board_fail_note(site,value,caller);board_fail(NULL);}
int a733_counter_clock_now(void *context,uint64_t *out)
{assert(context==&providers.shared.counter);*out=now_value;return mode!=15;}
int npu_runtime_send(struct npu_runtime *r,unsigned slot,enum npu_service_operation op,unsigned model,
                     uint64_t timeout,TickType_t wait,uint64_t *ticket)
{
    assert(r==&npu.runtime&&!slot&&op==NPU_SERVICE_LOAD&&model==2&&!timeout&&wait);
    ++send_calls;if(mode==1)return 0;*ticket=sent_ticket;return 1;
}
int npu_runtime_collect(struct npu_runtime *r,unsigned slot,uint64_t ticket,
                        struct npu_service_reply *reply,TickType_t wait)
{
    assert(r==&npu.runtime&&!slot&&ticket==sent_ticket&&wait);
    ++collect_calls;if(mode==2)return 0;
    *reply=(struct npu_service_reply){.ticket=ticket,.result=NPU_SERVICE_OK,
        .fault=A733_NPU_NO_FAULT,.state=A733_NPU_READY};
    if(mode==3)++reply->ticket;
    if(mode==4)reply->result=NPU_SERVICE_DENIED;
    if(mode==5)reply->fault=A733_NPU_TIMEOUT;
    if(mode==6)reply->state=A733_NPU_RUNNING;
    if(mode==7)live_value=0;
    if(mode==8)board_fail_note(0xa234,0xfedcba98,(const void *)(uintptr_t)0x40012340);
    if(mode==9)reply->ticket+=UINT64_C(1)<<40;
    return 1;
}
BaseType_t xTaskGenericNotify(TaskHandle_t task,UBaseType_t index,uint32_t value,eNotifyAction action,uint32_t *prior)
{(void)task;(void)index;(void)value;(void)action;(void)prior;return pdPASS;}
uint32_t ulTaskGenericNotifyTake(UBaseType_t index,BaseType_t clear,TickType_t wait)
{(void)index;(void)clear;(void)wait;abort();}
void vTaskSuspend(TaskHandle_t task){assert(!task);longjmp(suspended,1);}
void vTaskDelay(TickType_t ticks){(void)ticks;abort();}
enum a733_npu_platform_result npu_runtime_startup_poll(struct npu_runtime *r)
{assert(r==&npu.runtime);return mode==11?A733_NPU_PLATFORM_RETAINED:A733_NPU_PLATFORM_READY;}
enum npu_service_result npu_service_activate(struct npu_service *s)
{assert(s==&npu.runtime.service);return mode==12?NPU_SERVICE_DENIED:NPU_SERVICE_OK;}
enum a733_npu_platform_result npu_runtime_stop_poll(struct npu_runtime *r)
{assert(r==&npu.runtime);return A733_NPU_PLATFORM_RETAINED;}
uint32_t a733_npu_startup_wait_ticks(struct a733_npu_startup *s)
{(void)s;abort();}
static void emit_record(void)
{printf("%u %u %u %u\n",first_site,first_value,(uint32_t)os_npu_first_failure.ticket,os_npu_first_failure.operation);}
int main(int argc,char **argv)
{
    assert(argc==2);mode=(unsigned)strtoul(argv[1],NULL,10);
    npu.started=1;npu.end=1000000;npu.initial_deadline=2000;
    atomic_store(&npu.gate,(UINT64_C(900000)<<3)|ALLOW);
    atomic_store(&npu.runtime.slot[0].state,NPU_RUNTIME_READY);
    if(mode==10)npu.end=now_value+1;
        if(mode==14)live_value=0;
    if(mode==16)now_value=npu.end;
    if(mode==17)atomic_store(&npu.gate,(now_value<<3)|STOP);
    if(mode==18)npu.started=0;
    if(mode==19)atomic_store(&npu.failed,1);
    if(mode>=11&&mode<=13) {
        if(mode==13)atomic_store(&npu.closed,1);
        if(!setjmp(suspended))bootstrap(NULL);
        assert(board_failures==1&&atomic_load(&os_npu_first_failure.ready)==2);
        unsigned op=mode==13?NPU_SERVICE_CLOSE:NPU_SERVICE_OPEN;
        assert(os_npu_first_failure.operation==op);
        assert(os_npu_first_failure.observed==(mode==12?NPU_SERVICE_DENIED:A733_NPU_PLATFORM_RETAINED));
        assert(!send_calls&&!collect_calls);emit_record();return 0;
    }
    const int result=call(NPU_SERVICE_LOAD,2,0);
    if(mode==0||mode==8) {
        assert(result==1&&!board_failures&&!atomic_load(&os_npu_first_failure.ready));
        if(mode==8) {
            (void)FAIL_REQUEST(NPU_SERVICE_LOAD,2,sent_ticket,NULL,1,99);
            assert(first_site==0xa234&&first_value==0xfedcba98&&first_caller==(const void *)(uintptr_t)0x40012340);
        }
        emit_record();return 0;
    }
    assert(result==-1&&board_failures==1&&atomic_load(&npu.failed));
    const struct os_npu_failure *f=&os_npu_first_failure;
    assert(atomic_load(&f->ready)==2&&f->operation==NPU_SERVICE_LOAD&&f->model==2);
    assert(f->ticket==(mode==1||mode==10||mode>=14?0:sent_ticket));
    assert(f->reply_valid==(mode>=3&&mode<=9));
    assert(first_site==f->site&&first_value==(uint32_t)f->observed);
    assert((first_site&0xf000)==SUNSTONE_CRUMB_FAIL_NPU<<12);
    assert(first_site&SUNSTONE_CRUMB_NPU_REQUEST);
    assert((first_site>>24&7)==NPU_SERVICE_LOAD&&(first_site>>16&255)==(f->ticket&255));
    if(mode==3||mode==9)assert(f->expected==sent_ticket&&f->observed==sent_ticket+(mode==3?1:UINT64_C(1)<<40));
    if(mode==4)assert(f->expected==NPU_SERVICE_OK&&f->observed==NPU_SERVICE_DENIED);
    if(mode==5)assert(f->expected==A733_NPU_NO_FAULT&&f->observed==A733_NPU_TIMEOUT);
    if(mode==6)assert(f->expected==A733_NPU_READY&&f->observed==A733_NPU_RUNNING);
    if(mode==7)assert(f->watch_valid&&f->expected==1&&f->observed==0);
    if(mode==14||mode==15||mode==18)assert(f->watch_valid&&f->expected==1&&f->observed==0);
    if(mode==16)assert(f->watch_valid&&f->now==npu.end&&f->expected==npu.end&&f->observed==npu.end);
    if(mode==17)assert(f->watch_valid&&f->now==now_value&&f->expected==now_value&&f->observed==now_value&&(f->gate&STOP));
    if(mode==19)assert(f->watch_valid&&f->expected==0&&f->observed==1);
    if(mode==10)assert(!send_calls&&!collect_calls&&f->observed==0);
    /* Later downstream refusal preserves both the original RTC latch and
     * the first local native snapshot, including full 64-bit operands. */
    uint32_t saved=first_site;uint64_t observed=f->observed;
    (void)FAIL_REQUEST(NPU_SERVICE_CLOSE,7,4,NULL,0,123);
    assert(first_site==saved&&f->observed==observed&&f->operation==NPU_SERVICE_LOAD);
    emit_record();
}
