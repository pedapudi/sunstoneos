/* Optional OS catalogue runtime. The board owns every power/clock provider;
 * this consumer owns one fixed RAM reservation, startup row and client slot. */
#include "os.h"
#include "npu.h"
#include "../board/application.h"
#include "a733_mapping_check.h"
#include "aarch64_device_memory.h"
#include "device_memory_pool.h"
#include "app_large.h"
#include "sunstone_crumb.h"
#include "boot.h"
#include "sunstone_npu_service.h"
#include "sunstone_npu_queue.h"
#include <string.h>
#define ALLOW UINT64_C(1)
#define ACCEPTED UINT64_C(2)
#define STOP UINT64_C(4)
#define STACK 4096u
static struct os_npu_application {
    struct npu_runtime runtime;
    struct a733_npu_startup startup;
    struct device_memory_pool pool;
    struct device_memory_lease lease;
    struct device_memory_extent extent;
    StaticTask_t tasks[2];StackType_t stacks[2][STACK];
    TaskHandle_t bootstrap,client;
    atomic_uint_fast64_t gate;
    atomic_uint active,closed,stopped,failed;
    uint64_t initial_deadline,end;
    unsigned attempted,started,opened,jobs;
    uint32_t window_ms; /* the reply wait window of the last call */
} npu;
static atomic_uint bootstrap_ran; /* The bootstrap task has run: the service may have powered or submitted. */
static unsigned creating; /* Only while os_npu_start creates its two tasks. */
extern unsigned char __os_npu_pool_start[],__os_npu_pool_end[];
static int clock_now(uint64_t *now)
{return a733_counter_clock_now(&providers.shared.counter,now);}
struct os_npu_failure os_npu_first_failure;
struct os_npu_fault_info os_npu_last_fault;
#ifdef SUNSTONE_NPU_FAULT_HOOK
/* NEGATIVE-TEST IMAGES ONLY ("NPU MMU exceptions" in docs/reference-backsolve-npu.md). A DRAM buffer that this image
 * owns, that the NPU page tables do not map, and that holds a known pattern. The test hook in nbg_assembly.c aims one
 * descriptor pointer 64 KiB into it, so an NPU access that bypasses the MMU reads the pattern or changes the buffer.
 * The failure record's caller word carries the CRC-32 of the whole buffer at that moment. The pattern word i is
 * i * 0x9e3779b1 xor 0xc0ffee11. */
#define CANARY_BYTES (1u<<20)
unsigned char sunstone_npu_canary[CANARY_BYTES] __attribute__((aligned(4096)));
static uint32_t crc32_of(const unsigned char *p,size_t n)
{
    uint32_t crc=UINT32_MAX;
    while(n--){crc^=*p++;for(unsigned i=0;i<8;++i)crc=crc>>1^(UINT32_C(0xedb88320)&(0u-(crc&1u)));}
    return ~crc;
}
static void canary_prepare(void)
{
    for(uint32_t i=0;i<CANARY_BYTES/4;++i){
        const uint32_t v=i*UINT32_C(0x9e3779b1)^UINT32_C(0xc0ffee11);
        memcpy(sunstone_npu_canary+4*(size_t)i,&v,4);
    }
    (void)aarch64_device_cache(AARCH64_CACHE_CLEAN_INVALIDATE,(uintptr_t)sunstone_npu_canary,CANARY_BYTES);
}
static uint32_t canary_crc(void)
{
    (void)aarch64_device_cache(AARCH64_CACHE_INVALIDATE,(uintptr_t)sunstone_npu_canary,CANARY_BYTES);
    return crc32_of(sunstone_npu_canary,CANARY_BYTES);
}
#define RECORD_WHERE() ((const void *)(uintptr_t)canary_crc())
/* The test case of the next LOAD (read by the assembler, drivers/npu/nbg_assembly.c): a descriptor pointer slot
 * (1 first input, 2 output; negative for none) and a device address (0: 64 KiB into the canary). An image that lists
 * its cases fixes one for each catalogue entry; the host checks set the pair themselves. */
int sunstone_npu_hook_slot=-1;
uint32_t sunstone_npu_hook_address;
#ifdef SUNSTONE_NPU_CASE_SLOTS
static const int case_slots[]={SUNSTONE_NPU_CASE_SLOTS};
static const uint32_t case_addresses[]={SUNSTONE_NPU_CASE_ADDRESSES};
#endif
#else
#define RECORD_WHERE() SUNSTONE_CRUMB_HERE()
#endif
struct watch_failure {
    uint32_t line;
    uint64_t expected,observed,now,end,gate;
    const void *where;
};
static int fail_at(uint32_t line,enum npu_service_operation op,unsigned model,uint64_t ticket,
                   const struct npu_service_reply *reply,uint64_t expected,uint64_t observed,const void *where,
                   const struct watch_failure *watch)
{
    const uint32_t site=SUNSTONE_CRUMB_FAIL_NPU<<12|(line&0xfffu)|SUNSTONE_CRUMB_NPU_REQUEST|
        ((uint32_t)op&7u)<<24|((uint32_t)ticket&255u)<<16;
    /* First-note ownership belongs to the existing board latch. In particular,
     * a native register or startup failure must survive this caller's refusal. */
    board_fail_note(site,(uint32_t)observed,where);
    atomic_store_explicit(&npu.failed,1,memory_order_release);
    unsigned empty=0;
    if(atomic_compare_exchange_strong_explicit(&os_npu_first_failure.ready,&empty,1,
            memory_order_acq_rel,memory_order_acquire)) {
        struct os_npu_failure *f=&os_npu_first_failure;
        f->site=site;f->operation=op;f->model=model;f->ticket=ticket;
        f->expected=expected;f->observed=observed;f->caller=(uintptr_t)where;
        f->reply_valid=reply!=NULL;if(reply)f->reply=*reply;
        if(watch){f->watch_valid=1;f->now=watch->now;f->end=watch->end;f->gate=watch->gate;}
        /* These fields have atomic publication. A timeout does not authorize
         * reading mutable controller fields owned by the NPU worker. */
        f->slot_state=atomic_load_explicit(&npu.runtime.slot[0].state,memory_order_acquire);
        f->runtime_failed=atomic_load_explicit(&npu.runtime.failed,memory_order_acquire);
        f->irq_pending=atomic_load_explicit(&npu.runtime.service.irq_pending,memory_order_acquire);
        f->irq_stopped=atomic_load_explicit(&npu.runtime.service.irq_stopped,memory_order_acquire);
        atomic_store_explicit(&f->ready,2,memory_order_release);
    }
    board_fail_at(site,(uint32_t)observed,where);return -1;
}
#define FAIL_REQUEST(op,model,ticket,reply,expected,observed) \
    fail_at(__LINE__,(op),(model),(ticket),(reply),(uint64_t)(expected),(uint64_t)(observed),SUNSTONE_CRUMB_HERE(),NULL)
static int fail_watch(enum npu_service_operation op,unsigned model,uint64_t ticket,
                      const struct npu_service_reply *reply,const struct watch_failure *watch)
{return fail_at(watch->line,op,model,ticket,reply,watch->expected,watch->observed,watch->where,watch);}
static void sdk_notify(void *unused)
{(void)unused;if(npu.client)xTaskNotifyGive(npu.client);}
#ifdef SUNSTONE_A733_OS_NPU_QUEUE
void os_npu_queue_notify(void){sdk_notify(NULL);}
#endif
static int fail(void)
{sdk_notify(NULL);return FAIL_REQUEST(NPU_SERVICE_STATUS,UINT32_MAX,0,NULL,0,npu.runtime.native.failed);}
static void fatal(void *v){(void)v;(void)fail();}
static int live(void)
{return npu.started&&board_live(NULL)&&!atomic_load_explicit(&npu.failed,memory_order_acquire);}
static uint64_t capped(uint64_t now)
{return now>npu.end||npu.end-now<5000000?npu.end:now+5000000;}
void os_npu_thermal(int allow,int stop,uint64_t until,uint64_t now)
{
    if(!npu.started)return;
    uint64_t old=atomic_load_explicit(&npu.gate,memory_order_acquire),next;
    do {
        if(old&STOP)return;
        uint64_t end=stop?capped(now):until;
        if(end>UINT64_MAX>>3)return;
        next=(end<<3)|(old&ACCEPTED)|(stop?STOP:allow?ALLOW:0);
    } while(!atomic_compare_exchange_weak_explicit(&npu.gate,&old,next,memory_order_acq_rel,memory_order_acquire));
    /* Freshness wakes the idle mailbox owner for its own health/deadline
     * check. Request publication independently wakes it without a tick. */
    if(npu.client)xTaskNotifyGive(npu.client);
    if(!((old^next)&(STOP|ALLOW)))return;
    if(npu.bootstrap)xTaskNotifyGive(npu.bootstrap);
}
/* Same packed publication/claim ordering as the NPU board client: no mixed
 * generation timestamp, STOP survives completion and a preempted claim must
 * recheck time before issuing work. */
static void complete(void){atomic_fetch_and_explicit(&npu.gate,~ACCEPTED,memory_order_release);}
static int claim(void)
{
    uint64_t word=atomic_load_explicit(&npu.gate,memory_order_acquire),now;
    for(unsigned i=0;i<8;++i) {
        if((word&7)!=ALLOW||!clock_now(&now)||now>=word>>3)return 0;
        if(atomic_compare_exchange_weak_explicit(&npu.gate,&word,word|ACCEPTED,memory_order_acq_rel,memory_order_acquire)) {
            if(clock_now(&now)&&now<word>>3)return 1;
            complete();return 0;
        }
    }
    return 0;
}
#define WATCH_REFUSE(want,got) do { \
    *why=(struct watch_failure){.line=__LINE__,.expected=(uint64_t)(want),.observed=(uint64_t)(got), \
        .now=sampled,.end=npu.end,.gate=gate_sampled?word:atomic_load_explicit(&npu.gate,memory_order_acquire), \
        .where=SUNSTONE_CRUMB_HERE()};return 0; \
} while(0)
static int watch(uint64_t *now,struct watch_failure *why)
{
    uint64_t sampled=0,word=0;unsigned gate_sampled=0;
    if(!npu.started)WATCH_REFUSE(1,0);
    const unsigned failed=atomic_load_explicit(&npu.failed,memory_order_acquire);
    if(failed)WATCH_REFUSE(0,failed);
    if(!board_live(NULL))WATCH_REFUSE(1,0);
    if(!clock_now(now))WATCH_REFUSE(1,0);
    sampled=*now;
    if(*now>=npu.end)WATCH_REFUSE(npu.end,*now);
    word=atomic_load_explicit(&npu.gate,memory_order_acquire);gate_sampled=1;
    const uint64_t until=word>>3;
    if(!(word&STOP)&&*now>=until&&*now-until>=4000000) {
        os_npu_thermal(0,1,0,*now);
        word=atomic_load_explicit(&npu.gate,memory_order_acquire);
    }
    if((word&STOP)&&*now>=word>>3)WATCH_REFUSE(word>>3,*now);
    return 1;
}
#undef WATCH_REFUSE
static int progress(uint64_t *now)
{
    struct watch_failure why;
    if(!watch(now,&why))return fail_watch(NPU_SERVICE_STATUS,UINT32_MAX,0,NULL,&why);
    BOARD_PROGRESS(OS_SERVICE_NPU);return 1;
}
int os_npu_progress(void)
{uint64_t now;return progress(&now);}
int os_npu_stopping(void)
{return !live()||(atomic_load_explicit(&npu.gate,memory_order_acquire)&STOP)!=0;}
unsigned os_npu_stopped(void)
{
    /* A late IRQ or worker fault revokes the stopped proof even after the
     * finite client has published its immutable report and suspended. */
    return atomic_load_explicit(&npu.stopped,memory_order_acquire)&&
        !atomic_load_explicit(&npu.failed,memory_order_acquire)&&
        !atomic_load_explicit(&npu.runtime.failed,memory_order_acquire)&&
        !atomic_load_explicit(&npu.runtime.service.irq_pending,memory_order_acquire)&&
        (!atomic_load_explicit(&npu.active,memory_order_acquire)||
         (atomic_load_explicit(&npu.runtime.service.irq_stopped,memory_order_acquire)&&
          npu_service_runtime_parked(&npu.runtime.service)));
}
static int retirement_fault(void)
{
    if(atomic_load_explicit(&npu.closed,memory_order_acquire)||
       atomic_load_explicit(&npu.stopped,memory_order_acquire)){
        unsigned late=atomic_load_explicit(&npu.runtime.service.irq_pending,memory_order_acquire);
        if(atomic_load_explicit(&npu.runtime.failed,memory_order_acquire))late|=1u<<8;
        if(atomic_load_explicit(&npu.active,memory_order_acquire)&&
           !atomic_load_explicit(&npu.runtime.service.irq_stopped,memory_order_acquire))late|=1u<<9;
        if(atomic_load_explicit(&npu.failed,memory_order_acquire))late|=1u<<10;
        if(late){(void)FAIL_REQUEST(NPU_SERVICE_CLOSE,UINT32_MAX,0,NULL,0,late);return 1;}
    }
    return 0;
}
/* The service never ran: its bootstrap task did not start and no activation
 * happened, so nothing was powered, clocked or submitted and nothing is
 * retained. Startup that refuses before the scheduler is this case; its
 * allocations are DRAM state that the host restores. */
int os_npu_never_ran(void)
{return !atomic_load_explicit(&bootstrap_ran,memory_order_acquire)&&!atomic_load_explicit(&npu.active,memory_order_acquire);}
int os_npu_retirement_check(void)
{return !retirement_fault()&&os_npu_complete();}
#ifdef SUNSTONE_A733_OS_PERF
/* Performance modes: the request number and rate in one word, the answer
 * number and result in another; the bootstrap serves requests only after the
 * local stop. */
static atomic_uint perf_request,perf_answer,perf_ready;
static uint64_t perf_deadline; /* Single thermal producer; request owns it until answer. */
int os_npu_perf_ready(void)
{return atomic_load_explicit(&perf_ready,memory_order_acquire) && !atomic_load_explicit(&npu.failed,memory_order_acquire);}
int os_npu_perf_stopped(void){return os_npu_perf_ready() && os_npu_stopped();}
int os_npu_perf_retune(unsigned mhz,unsigned sequence,uint64_t deadline)
{
    uint64_t now;
    if(!npu.bootstrap || !live() || !os_npu_perf_stopped() || (mhz!=492 && mhz!=852) ||
       !sequence || sequence>0xffff || atomic_load_explicit(&perf_request,memory_order_acquire) ||
       !clock_now(&now) || now>=deadline || deadline>board.session.deadline_us-10000)return 0;
    perf_deadline=deadline;
    atomic_store_explicit(&perf_request,sequence<<16|mhz,memory_order_release);
    xTaskNotifyGive(npu.bootstrap);return 1;
}
unsigned os_npu_perf_answer(unsigned sequence)
{
    const unsigned a=atomic_load_explicit(&perf_answer,memory_order_acquire);
    return a>>2==sequence ? (a&3) : 0;
}
/* Bootstrap task after the local stop: one request, then wait for the next. */
static void perf_serve(void)
{
    for(;;) {
        (void)ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
        const unsigned request=atomic_load_explicit(&perf_request,memory_order_acquire);
        if(!request)continue;
        uint64_t now;
        const int r=!live() || !os_npu_perf_stopped() ? -1 :
            !clock_now(&now) ? -1 : now>=perf_deadline ? 0 :
            a733_npu_startup_retune_until(&npu.startup,request&0xffff,perf_deadline);
        atomic_store_explicit(&perf_answer,(request>>16)<<2|(r==1 ? 1u : r==0 ? 2u : 3u),memory_order_release);
        atomic_store_explicit(&perf_request,0,memory_order_release);
        if(r<0){(void)FAIL_REQUEST(NPU_SERVICE_STATUS,UINT32_MAX,0,NULL,1,r);vTaskSuspend(NULL);}
    }
}
#define STOPPED_WAIT() perf_serve()
#else
#define STOPPED_WAIT() vTaskSuspend(NULL)
#endif
static int caller(void)
{return live()&&npu.client&&xTaskGetSchedulerState()==taskSCHEDULER_RUNNING&&xTaskGetCurrentTaskHandle()==npu.client;}
/* A stalled or mismatched reply retains the outstanding runtime ticket;
 * wrappers never retry a submission or free its windows. */
static int call(enum npu_service_operation op,unsigned model,uint64_t timeout)
{
    uint64_t now=0,ticket=0;
    struct npu_service_reply reply={0};
    struct watch_failure why;
    if(!watch(&now,&why))return fail_watch(op,model,0,NULL,&why);
    BOARD_PROGRESS(OS_SERVICE_NPU);
    uint64_t end=npu.end,word=atomic_load_explicit(&npu.gate,memory_order_acquire);
    if((word&STOP)&&(word>>3)<end)end=word>>3;
    if(end<=now)return FAIL_REQUEST(op,model,0,NULL,end,now);
    #ifdef SUNSTONE_NPU_FAULT_HOOK
    TickType_t wait=pdMS_TO_TICKS(8000); /* a test image waits for the controller's own verdict on a stalled job */
#else
    TickType_t wait=pdMS_TO_TICKS(2000);
#endif
    const uint64_t ticks=(end-now)*configTICK_RATE_HZ/1000000;
    if(ticks<wait)wait=(TickType_t)ticks;
    if(!wait)return FAIL_REQUEST(op,model,0,NULL,1,ticks);
    npu.window_ms=(uint32_t)((uint64_t)wait*1000u/configTICK_RATE_HZ);
    if(op==NPU_SERVICE_SUBMIT&&npu.runtime.service.runtime)npu.runtime.service.runtime->marks=0;
    if(!npu_runtime_send(&npu.runtime,0,op,model,timeout,wait,&ticket))
        return FAIL_REQUEST(op,model,0,NULL,1,atomic_load_explicit(&npu.runtime.slot[0].state,memory_order_acquire));
    if(npu.runtime.service.runtime)npu.runtime.service.runtime->marks|=NPU_MARK_COLLECT;
    if(!npu_runtime_collect(&npu.runtime,0,ticket,&reply,wait)) {
#ifdef SUNSTONE_NPU_FAULT_HOOK
        /* No reply in time. The record keeps the progress marks (high half of the value, the worker's steps from the
         * MMU capture to the reply and the client's wait) and the wait window in ms (low half, capped), the capture
         * stage (low nibble of the ticket byte) and the controller fault code (high nibble). The controller fields are
         * read from another core, for this image only. */
        const struct a733_npu_controller *c=&npu.runtime.service.controller;
        const uint32_t marks=npu.runtime.service.runtime?npu.runtime.service.runtime->marks:0;
        return fail_at(__LINE__,op,model,(c->mmu_stage&15u)|((unsigned)c->fault&15u)<<4,NULL,1,
                       (uint64_t)(marks&0xffffu)<<16|(npu.window_ms>0xffffu?0xffffu:npu.window_ms),RECORD_WHERE(),NULL);
#endif
        return FAIL_REQUEST(op,model,ticket,NULL,1,atomic_load_explicit(&npu.runtime.slot[0].state,memory_order_acquire));
    }
    if(npu.runtime.service.runtime)npu.runtime.service.runtime->marks|=NPU_MARK_WOKE;
    if(reply.ticket!=ticket)return FAIL_REQUEST(op,model,ticket,&reply,ticket,reply.ticket);
    if(reply.result==NPU_SERVICE_JOB_FAULT&&reply.fault==A733_NPU_MMU&&op==NPU_SERVICE_SUBMIT) {
        /* An NPU MMU exception failed this job; the service recovers the NPU and the session goes on. The fault is
         * published for the application (os_npu_last_fault) and the call returns 2. */
        if(npu.runtime.service.runtime)npu.runtime.service.runtime->marks|=NPU_MARK_READ;
        os_npu_last_fault.mmu_status=reply.mmu_status;os_npu_last_fault.mmu_address=reply.mmu_address;
        os_npu_last_fault.marks=npu.runtime.service.runtime?npu.runtime.service.runtime->marks:0;
        os_npu_last_fault.window_ms=npu.window_ms;
#ifdef SUNSTONE_NPU_FAULT_HOOK
        os_npu_last_fault.canary_crc=canary_crc();
#endif
        return 2;
    }
    if(reply.result!=NPU_SERVICE_OK)return FAIL_REQUEST(op,model,ticket,&reply,NPU_SERVICE_OK,reply.result);
    if(reply.fault!=A733_NPU_NO_FAULT)return FAIL_REQUEST(op,model,ticket,&reply,A733_NPU_NO_FAULT,reply.fault);
    /* The runtime service answers OPEN as soon as the controller bootstrap
     * starts (BOOTING) or, when it has already completed, READY. The next
     * LOAD is held until its own completion, so BOOTING is not final. The
     * runtime accepts no WAIT (npu_runtime_send), unlike the finite client. */
    const enum a733_npu_state expected=op==NPU_SERVICE_CLOSE?A733_NPU_CLOSED:
        op==NPU_SERVICE_SUBMIT?A733_NPU_DONE:A733_NPU_READY;
    if(reply.state!=expected&&!(op==NPU_SERVICE_OPEN&&reply.state==A733_NPU_BOOTING))return FAIL_REQUEST(op,model,ticket,&reply,expected,reply.state);
    if(!watch(&now,&why))return fail_watch(op,model,ticket,&reply,&why);
    return 1;
}
int os_npu_open(void)
{
    if(!caller()||npu.opened)return 0;
    if(!atomic_load_explicit(&npu.active,memory_order_acquire))return 0;
#ifdef SUNSTONE_NPU_FAULT_HOOK
    canary_prepare();
#endif
    if(call(NPU_SERVICE_OPEN,0,1000000)!=1)return -1;
    npu.opened=1;return 1;
}
int os_npu_register(const void *model,size_t bytes,unsigned *entry)
{
    /* The graph is admitted like a linked one and must fit the bounds declared in the session (npu_runtime_register). */
    return caller()&&entry&&npu_runtime_register(&npu.runtime,model,bytes,entry);
}
int os_npu_load(unsigned entry)
{
    if(!caller()||!npu.opened||entry>=atomic_load_explicit(&npu.runtime.count,memory_order_acquire)||os_npu_stopping())return 0;
#ifdef SUNSTONE_NPU_CASE_SLOTS
    if(entry<sizeof(case_slots)/sizeof(*case_slots)){sunstone_npu_hook_slot=case_slots[entry];sunstone_npu_hook_address=case_addresses[entry];}
    else sunstone_npu_hook_slot=-1;
#endif
    return call(NPU_SERVICE_LOAD,entry,0);
}
int os_npu_tensor(enum nbg_section_id section,unsigned index,struct npu_runtime_tensor *tensor)
{
    if(!caller()||!npu.opened)return 0;
    return npu_runtime_tensor(&npu.runtime,0,section,index,tensor);
}
#ifdef SUNSTONE_A733_OS_NPU_QUEUE
struct npu_runtime *os_npu_queue_runtime(void)
{return caller()&&npu.opened?&npu.runtime:NULL;}
int os_npu_queue_begin(void)
{return caller()&&npu.opened&&claim();}
void os_npu_queue_end(void){complete();}
#endif
int os_npu_submit(uint64_t timeout)
{
    const struct os_session *s=os_session();
    if(!caller()||!npu.opened||!timeout||timeout>1000000||!s||!claim())return 0;
    int result=call(NPU_SERVICE_SUBMIT,0,timeout);
    complete();
#ifdef SUNSTONE_NPU_FAULT_HOOK
    /* The job completed although a descriptor pointer is unmapped: the access did not fault. The record (ticket byte
     * 0xee) carries the canary CRC as the value and the CRC of the first output tensor as the caller word; the tests
     * of the host and the board compare both with the pattern and with the reference output. */
    if(result==1&&sunstone_npu_hook_slot>=0){
        struct npu_runtime_tensor t;uint32_t out=0;
        if(os_npu_tensor(NBG_OUTPUT,0,&t)==1)out=crc32_of(t.data,t.bytes);
        return fail_at(__LINE__,NPU_SERVICE_SUBMIT,0,0xee,NULL,0,canary_crc(),(const void *)(uintptr_t)out,NULL);
    }
#endif
    if(result==1)++npu.jobs;return result;
}
int os_npu_close(void)
{
    if(!caller()||!npu.opened||atomic_load_explicit(&npu.closed,memory_order_acquire))return 0;
    uint64_t now;if(!clock_now(&now))return FAIL_REQUEST(NPU_SERVICE_CLOSE,0,0,NULL,1,0);
    os_npu_thermal(0,1,0,now);
    if(call(NPU_SERVICE_CLOSE,0,0)!=1)return -1;
    npu.opened=0;atomic_store_explicit(&npu.closed,1,memory_order_release);
    xTaskNotifyGive(npu.bootstrap);return 1;
}
static void bootstrap(void *unused)
{
    (void)unused;unsigned begun=0;
    atomic_store_explicit(&bootstrap_ran,1,memory_order_release);
    for(;;) {
        uint64_t now;struct watch_failure why;
        if(!watch(&now,&why)){(void)fail_watch(NPU_SERVICE_STATUS,UINT32_MAX,0,NULL,&why);goto failed;}
        if(!begun) {
            if(now>=npu.initial_deadline)os_npu_thermal(0,1,0,now);
            if(os_npu_stopping()) {
                /* No startup step or activation has occurred. */
                atomic_store_explicit(&npu.stopped,1,memory_order_release);
                xTaskNotifyGive(npu.client);STOPPED_WAIT();return;
            }
            if(!claim()){(void)ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(25));continue;}
            complete();begun=1;
        }
        enum a733_npu_platform_result r=npu_runtime_startup_poll(&npu.runtime);
        if(r==A733_NPU_PLATFORM_RETAINED){(void)FAIL_REQUEST(NPU_SERVICE_OPEN,0,0,NULL,A733_NPU_PLATFORM_READY,r);goto failed;}
        if(r==A733_NPU_PLATFORM_READY) {
            enum npu_service_result activated=npu_service_activate(&npu.runtime.service);
            if(activated==NPU_SERVICE_OK) {
#ifdef SUNSTONE_A733_OS_PERF
                atomic_store_explicit(&perf_ready,1,memory_order_release);
#endif
                atomic_store_explicit(&npu.active,1,memory_order_release);xTaskNotifyGive(npu.client);
                for(;;) {
                    if(!watch(&now,&why)){(void)fail_watch(NPU_SERVICE_STATUS,UINT32_MAX,0,NULL,&why);goto failed;}
                    if(atomic_load_explicit(&npu.closed,memory_order_acquire)) {
                        /* The final reply precedes the worker's last callback.
                         * Its release acknowledgment and notification precede
                         * retiring startup/IRQ provider ownership. */
                        if(!npu_service_runtime_parked(&npu.runtime.service)){
                            (void)ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(25));continue;
                        }
                        if(retirement_fault())goto failed;
                        r=npu_runtime_stop_poll(&npu.runtime);
                        if(r==A733_NPU_PLATFORM_RETAINED){(void)FAIL_REQUEST(NPU_SERVICE_CLOSE,0,0,NULL,A733_NPU_PLATFORM_READY,r);goto failed;}
                        if(r==A733_NPU_PLATFORM_READY) {
                            atomic_store_explicit(&npu.stopped,1,memory_order_release);
                            xTaskNotifyGive(npu.client);STOPPED_WAIT();return;
                        }
                        vTaskDelay(1);
                    } else (void)ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(25));
                }
            }
            if(activated!=NPU_SERVICE_BUSY){(void)FAIL_REQUEST(NPU_SERVICE_OPEN,0,0,NULL,NPU_SERVICE_OK,activated);goto failed;}
        }
        /* A PENDING startup blocks until its power-service notification, its
         * deadline or the last tick of its settle; a thermal notification
         * ends the block early. */
        (void)ulTaskNotifyTake(pdTRUE,r==A733_NPU_PLATFORM_PENDING ? a733_npu_startup_wait_ticks(&npu.startup) : 1);
    }
failed:
    vTaskSuspend(NULL);
}
/* The bootstrap and the registered client call a733_counter_clock_now, which
 * masks DAIF, so both start at EL1t. Only their two creations in
 * os_npu_start qualify; any other task with these entries starts at EL0. */
int os_npu_task_privileged(void (*entry)(void *),void *context)
{return (creating&&!context&&(entry==bootstrap||entry==os_npu_client_task))||
    npu_service_task_privileged(entry,context)==pdTRUE;}
int os_npu_pins(struct a733_pin_handoff *pins){return pins&&os_session_admitted();}
/* Each start refusal records its own source line (group NPU, value 0). */
static int refused(uint32_t site,const void *here){board_fail_note(site,0,here);return 0;}
#define REFUSE() refused(SUNSTONE_CRUMB_FAIL_SITE(NPU),SUNSTONE_CRUMB_HERE())
/* What the image uses of the NPU: one slot for the synchronous wrapper, one for each graph under the queue. */
static struct npu_image_needs image_needs(void)
{
    return (struct npu_image_needs){
#ifdef SUNSTONE_A733_OS_NPU_QUEUE
        os_npu_model_count,os_npu_model_count,ss_npu_queue_edges.count
#else
        1,os_npu_model_count,0
#endif
    };
}
/* The tables this image holds (fixed when it is built). */
static struct npu_tables image_tables(void)
{
    /* Graphs have no table limit: their tables come from the reservation. */
    return (struct npu_tables){NPU_RUNTIME_SLOTS,UINT32_MAX,SS_NPU_QUEUE_MAX_DEPTH,
        SS_NPU_QUEUE_MAX_HANDLES*SS_NPU_QUEUE_MAX_TENSORS};
}
static struct npu_declaration declaration_of(const struct os_npu_session *n)
{
    return (struct npu_declaration){n->slots,n->in_flight,n->graphs,n->links,n->graph_bytes,n->tensor_bytes};
}
/* The in-flight depth the session declares: the deepest queue the application may open. */
unsigned os_npu_in_flight(void)
{
    const struct os_session *s=os_session();
    return s?s->npu.in_flight:0;
}
/* The tensor links the session declares: the most edges the application may choose at run time (ss_npu_queue_open_linked). */
unsigned os_npu_links(void)
{
    const struct os_session *s=os_session();
    return s?s->npu.links:0;
}
/* A refused declaration leaves its reason in the first-failure record, as the value of a record in group NPU. */
static enum npu_declaration_result refused_declaration(enum npu_declaration_result r)
{
    board_fail_note(SUNSTONE_CRUMB_FAIL_SITE(NPU),(uint32_t)r,SUNSTONE_CRUMB_HERE());
    return r;
}
/* A session that declares nothing is refused (NPU_DECLARATION_MISSING): there is no default. */
enum npu_declaration_result os_npu_declaration_admitted(const struct os_npu_session *n)
{
    const struct npu_declaration d=declaration_of(n);
    const struct npu_image_needs need=image_needs();
    const struct npu_tables tables=image_tables();
    const enum npu_declaration_result r=npu_declaration_check(&d,&need,&tables);
#ifndef SUNSTONE_A733_OS_NPU_QUEUE
    /* The synchronous wrapper has one client task, so exactly one slot. */
    if(r==NPU_DECLARATION_OK&&d.slots!=1)return refused_declaration(NPU_DECLARATION_SLOTS);
#endif
    return r==NPU_DECLARATION_OK?r:refused_declaration(r);
}
int os_npu_start(void)
{
    extern int os_npu_sdk_bind(const struct npu_runtime *);
    extern int os_npu_diagnostic_bind(struct npu_runtime *) __attribute__((weak));
    if(!ss_npu_service_bind_event(sdk_notify,NULL))return REFUSE();
#ifdef SUNSTONE_A733_OS_NPU_QUEUE
    if(!os_npu_queue_sdk_bind())return REFUSE();
#endif
    const struct os_session *s=os_session_admitted();
    if(!s)return REFUSE();
    if(npu.attempted)return REFUSE();
    if(board.session.deadline_us<=10000)return REFUSE();
    npu.attempted=1;
    const struct os_npu_session *g=&s->npu;
    static const struct a733_mapping_rule wb={A733_MAPPING_WRITE,1,{{A733_MAIR_NORMAL_WB,A733_SH_INNER}},0,1};
    uintptr_t start=(uintptr_t)__os_npu_pool_start,end=(uintptr_t)__os_npu_pool_end;
    /* The reservation fits any one catalogue entry; the runtime takes the
     * linked entry's exact requirement from its start. */
    if(!os_npu_model_count)return REFUSE();
    /* The application's declaration sets the slots; the reservation covers them all, so that nothing is allocated on
     * the job path. A declaration that does not fit names the resource in the record's value. */
    if(os_npu_declaration_admitted(g)!=NPU_DECLARATION_OK)return REFUSE();
    const struct npu_declaration declared=declaration_of(g);
    const unsigned slots=declared.slots;
    size_t need=0;
    /* Graphs beyond the linked ones are loaded at run time, within the bounds the session declares. */
    struct npu_runtime_bounds bounds={0};
    if(declared.graph_bytes&&declared.tensor_bytes&&declared.graphs>os_npu_model_count)
        bounds=(struct npu_runtime_bounds){declared.graphs-os_npu_model_count,declared.graph_bytes,declared.tensor_bytes,NULL};
#ifdef SUNSTONE_A733_OS_NPU_QUEUE
    /* A queue image keeps graph i resident in slot i: each slot is sized by its own graph, not by the largest. */
    static unsigned slot_model[NPU_RUNTIME_SLOTS];
    if(!bounds.capacity&&slots==os_npu_model_count&&slots<=NPU_RUNTIME_SLOTS){
        for(unsigned i=0;i<slots;++i)slot_model[i]=i;
        bounds.slot_model=slot_model;
    }
#endif
    /* The lease is the whole pool and prepare takes what the declaration needs from its start (it refuses if that is more).
     * Sizing the reservation first would admit every graph twice before the scheduler, which the secondary cores wait for
     * one second at most: a layout takes tens of milliseconds on a Cortex-A55 at the boot clock. */
    need=OS_NPU_POOL_BYTES;
    if(start!=OS_NPU_POOL_START||end-start!=OS_NPU_POOL_BYTES)return REFUSE();
#ifdef OS_NPU_POOL_BASE
    /* A pool in the slot 1 area is used only while the ledger holds it for the kernel: no application region, heap or
     * stack overlaps it and no application space maps it. */
    if(!os_large_kernel_reserved(start,end-start))return REFUSE();
#endif
    if(!a733_mapping_check_stride(aarch64_device_translate,start,start,end-start,0x200000,&wb))return REFUSE();
    if(!device_memory_pool_init_typed(&npu.pool,start,start,end-start,DEVICE_MEMORY_CLASS_DMA32,DEVICE_MEMORY_WRITE_BACK))return REFUSE();
    if(!device_memory_acquire(&npu.pool,g->owner,need,&npu.lease,&npu.extent))return REFUSE();
    if(npu.extent.cpu!=start||npu.extent.physical!=start||npu.extent.bytes!=need)return REFUSE();
    struct npu_runtime_config config={.models=os_npu_models,.count=os_npu_model_count,.slots=slots,.bounds=bounds,.cpu=npu.extent.cpu,
        .physical=npu.extent.physical,.bytes=npu.extent.bytes,.register_cpu=0x03600000,
        .register_physical=0x03600000,.fatal=fatal};
    if(!npu_runtime_prepare(&npu.runtime,&config)){
        /* Name the cause: the reservation says whether a graph is refused or the declaration does not fit the pool. */
        size_t needed=0;
        const enum npu_declaration_result fit=npu_runtime_reservation_with(os_npu_models,os_npu_model_count,slots,OS_NPU_POOL_BYTES,&needed,&bounds);
        if(fit!=NPU_DECLARATION_OK)board_fail_note(SUNSTONE_CRUMB_FAIL_SITE(NPU),(uint32_t)fit,SUNSTONE_CRUMB_HERE());
        device_memory_retain(&npu.pool,npu.lease);return REFUSE();
    }
    if(!os_npu_sdk_bind(&npu.runtime)){device_memory_retain(&npu.pool,npu.lease);return REFUSE();}
    device_memory_lookup(&npu.pool,npu.lease)->state=DEVICE_MEMORY_DEVICE;
    const struct a733_board_session *b=&s->board;
    npu.end=board.session.deadline_us-10000;
#ifdef SUNSTONE_BOARD_RESIDENT
    uint64_t begun;
    if(!clock_now(&begun)||begun>UINT64_MAX-OS_NPU_WORK_US)return REFUSE();
    if(npu.end>begun+OS_NPU_WORK_US)npu.end=begun+OS_NPU_WORK_US;
#endif
    const struct a733_npu_startup_config startup={{{b->fabric[0],b->fabric[1],b->fabric[2]},
        {b->fabric_hz[0],b->fabric_hz[1],b->fabric_hz[2]}},g->owner,npu.end,g->pck_client};
    const struct a733_npu_startup_providers p={&providers.shared.counter,&providers.shared.clocks,
        &providers.shared.locks,&providers.shared.gpu,&providers.shared.rails,&providers.domains};
    if(!a733_npu_startup_init(&npu.startup,&startup,&p,NULL,NULL))return REFUSE();
#if defined(SUNSTONE_A733_OS_PERF) && OS_PERF_BOOT_MODE==2
    /* The low boot mode: the NPU starts at its 492 MHz point. */
    if(!a733_npu_startup_rate(&npu.startup,492))return REFUSE();
#endif
    creating=1;
    npu.bootstrap=xTaskCreateStaticAffinitySet(bootstrap,"os-npu-boot",STACK,NULL,4,npu.stacks[0],&npu.tasks[0],2);
    npu.client=xTaskCreateStaticAffinitySet(os_npu_client_task,"os-npu-app",STACK,NULL,3,npu.stacks[1],&npu.tasks[1],2);
    creating=0;
#ifdef SUNSTONE_A733_OS_NPU_QUEUE
    /* The graphs are loaded when the ring opens, after the scheduler has started (npu_runtime_load_resident): hashing and
     * assembling megabytes would outlast the second that the secondary cores wait for the primary core to finish. */
#endif
#ifndef SUNSTONE_A733_OS_NPU_QUEUE
    TaskHandle_t clients[]={npu.client};
#endif
    uint64_t now;
    if(!npu.bootstrap||!npu.client)return REFUSE();
    if(!a733_npu_startup_bind_task(&npu.startup,npu.bootstrap))return REFUSE();
#ifdef SUNSTONE_A733_OS_NPU_QUEUE
    if(!npu_runtime_bind_resident(&npu.runtime,&npu.startup,npu.client,4,2))return REFUSE();
#else
    if(!npu_runtime_bind(&npu.runtime,&npu.startup,clients,4,2))return REFUSE();
#endif
    if(!os_npu_irq_bind(&npu.runtime))return REFUSE();
    if(os_npu_diagnostic_bind&&!os_npu_diagnostic_bind(&npu.runtime))return REFUSE();
    if(npu.end>(UINT64_MAX>>3)-5000000||!clock_now(&now))return REFUSE();
    if(now>=npu.end)return REFUSE();
    npu.initial_deadline=now+1000000;
    atomic_init(&npu.gate,npu.initial_deadline<<3);npu.started=1;return 1;
}
