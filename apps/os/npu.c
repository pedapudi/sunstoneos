/* Optional OS catalogue runtime. The board owns every power/clock provider;
 * this consumer owns one fixed RAM reservation, startup row and client slot. */
#include "os.h"
#include "perf.h"
#include "npu.h"
#include "../board/application.h"
#include "a733_mapping_check.h"
#include "aarch64_device_memory.h"
#include "umem.h"
#include "app_large.h"
#include "device-ranges.h"
#include "application.h"
#include "sunstone_crumb.h"
#include "boot.h"
#include "sunstone_npu_service.h"
#include "sunstone_npu_queue.h"
#include "npu-graph-index.h"
#include <string.h>
#define ALLOW UINT64_C(1)
#define ACCEPTED UINT64_C(2)
#define STOP UINT64_C(4)
#define STACK 4096u
static struct os_npu_application {
    struct npu_runtime runtime;
    struct a733_npu_startup startup;
    struct umem_buf pool;
    int prepared;
    StaticTask_t tasks[2];StackType_t stacks[2][STACK];
    TaskHandle_t bootstrap,client;
    atomic_uint_fast64_t gate;
    atomic_uint active,closed,stopped,failed;
    uint64_t initial_deadline,end;
    unsigned attempted,started,opened,jobs;
    uint32_t window_ms; /* the reply wait window of the last call */
} npu;
/* The EL1-private metadata of the runtime, the service and the queue adapter (include/npu_pool.h, struct npu_meta): memory that
 * no EL0 table maps and that the NPU's page tables do not map. It comes from kernel-owned ledger chunks, taken when a table is
 * asked for: at admission for the declaration's tables, and later for run-time links. A chunk starts with a header (the chunk
 * before it and its size); a block is carved from the newest chunk, and a block released last is given back to it. */
struct meta_chunk { struct meta_chunk *previous; size_t bytes, used; uint64_t padding[5]; };
_Static_assert(sizeof(struct meta_chunk)==64,"a chunk's blocks start on a 64-byte line");
static struct { struct npu_meta meta; struct umem *ledger; struct meta_chunk *newest; } meta_state;
static void *meta_allocate(struct npu_meta *m,size_t bytes)
{
    (void)m;
    bytes=(bytes+63u)&~(size_t)63;
    if(!meta_state.ledger||!bytes)return NULL;
    struct meta_chunk *c=meta_state.newest;
    if(!c||c->bytes-c->used<bytes){
        /* A new chunk: the request plus the header, in whole pages, and at least 64 KiB so that many small tables share it. */
        size_t want=(bytes+sizeof(struct meta_chunk)+4095u)&~(size_t)4095u;
        if(want<65536u)want=65536u;
        struct umem_buf b;
        struct umem_request r={.bytes=want,.align=4096,.owner=UMEM_OWNER_KERNEL,.flags=UMEM_RAW};
        if(umem_alloc(meta_state.ledger,&r,&b)!=UMEM_OK)return NULL;
        c=(struct meta_chunk *)(uintptr_t)b.base;
        c->previous=meta_state.newest;c->bytes=b.bytes-sizeof(struct meta_chunk);c->used=0;
        meta_state.newest=c;
    }
    void *block=(uint8_t *)c+sizeof(struct meta_chunk)+c->used;
    memset(block,0,bytes);
    c->used+=bytes;
    return block;
}
static void meta_release(struct npu_meta *m,void *block,size_t bytes)
{
    (void)m;
    bytes=(bytes+63u)&~(size_t)63;
    struct meta_chunk *c=meta_state.newest;
    if(c&&bytes<=c->used&&(uint8_t *)block==(uint8_t *)c+sizeof(struct meta_chunk)+c->used-bytes)c->used-=bytes;
}
static struct npu_meta *meta_init(struct umem *ledger)
{
    meta_state.meta.allocate=meta_allocate;meta_state.meta.release=meta_release;
    meta_state.ledger=ledger;meta_state.newest=NULL;
    return &meta_state.meta;
}
static atomic_uint bootstrap_ran; /* The bootstrap task has run: the service may have powered or submitted. */
static unsigned creating; /* Only while os_npu_start creates its two tasks. */
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
        f->slot_state=npu.runtime.slot?atomic_load_explicit(&npu.runtime.slot[0].state,memory_order_acquire):0; /* null before prepare */
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
void os_npu_queue_notify(void){sdk_notify(NULL);}
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
unsigned os_npu_retirement_state(void)
{
    unsigned s=OS_NPU_STATE_LINKED;
    if(!atomic_load_explicit(&os_npu_published_report.finished,memory_order_acquire))s|=OS_NPU_STATE_NOT_PUBLISHED;
    if(!os_npu_published_report.stopped)s|=OS_NPU_STATE_NOT_STOPPED_REPORT;
    if(os_npu_published_report.retained)s|=OS_NPU_STATE_REPORT_RETAINED;
    if(!os_npu_stopped())s|=OS_NPU_STATE_NOT_STOPPED;
    if(atomic_load_explicit(&npu.closed,memory_order_acquire))s|=OS_NPU_STATE_CLOSED;
    if(atomic_load_explicit(&npu.stopped,memory_order_acquire))s|=OS_NPU_STATE_STOPPED_FLAG;
    if(atomic_load_explicit(&npu.active,memory_order_acquire))s|=OS_NPU_STATE_ACTIVE;
    if(npu.started)s|=OS_NPU_STATE_STARTED;
    if(npu.opened)s|=OS_NPU_STATE_OPENED;
    if(atomic_load_explicit(&npu.failed,memory_order_acquire))s|=OS_NPU_STATE_FAILED;
    if(atomic_load_explicit(&npu.runtime.failed,memory_order_acquire))s|=OS_NPU_STATE_RUNTIME_FAILED;
    if(atomic_load_explicit(&npu.runtime.service.irq_pending,memory_order_acquire))s|=OS_NPU_STATE_IRQ_PENDING;
    if(atomic_load_explicit(&npu.runtime.service.irq_stopped,memory_order_acquire))s|=OS_NPU_STATE_IRQ_STOPPED;
    if(npu_service_runtime_parked(&npu.runtime.service))s|=OS_NPU_STATE_PARKED;
    return s;
}
int os_npu_retirement_check(void)
{return !os_granted(OS_GRANT_NPU)||(!retirement_fault()&&os_npu_complete());}
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
#define STOPPED_WAIT() do{if(os_perf_enabled())perf_serve();else vTaskSuspend(NULL);}while(0)
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
/* What taking the application's graphs cost, for the runner to copy out of image RAM: the number of graphs, the bytes copied,
 * and the microseconds in the copy, the container decode and the layout, summed over the graphs. Graphs copied out of the
 * declared region at admission are timed there (the copy, then the runtime's prepare, whose layout includes each graph's
 * decode: decode_us is zero for them); graphs registered later add the runtime's own counters (npu_runtime_counters). */
struct os_npu_register_record{uint64_t graphs,bytes,copy_us,decode_us,layout_us,reserved[3];};
struct os_npu_register_record os_npu_register_record;
static struct os_npu_register_record sealed_record;
/* Where admission's time went, for the runner to copy out of image RAM (apps/os/cpu-return-host.py decodes it as npu_prepare):
 * microseconds of the reservation, the runtime's prepare by part (npu_runtime_prepare_parts), what follows prepare, and the
 * whole; then the core's cycles and the generic counter's ticks over the whole, so the clock the work ran at is measured, and
 * the core's SCTLR_EL1 and MIDR_EL1 (the data cache state and the core). */
struct os_npu_prepare_record{uint64_t graphs,bytes,copy_us,reserve_us,decode_us,layout_us,carve_us,place_us,fill_us,native_us,
    finish_us,total_us,cycles,ticks,sctlr_el1,midr_el1;};
struct os_npu_prepare_record os_npu_prepare_record;
static uint64_t prepare_clock(void *unused){(void)unused;uint64_t t;return clock_now(&t)?t:0;}
#if defined(__aarch64__)
/* The PMU cycle counter, enabled for EL1 (the counter is per core): 0 when it cannot be had. */
static uint64_t cycles_now(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, pmccntr_el0":"=r"(v));
    return v;
}
static void cycles_enable(void)
{
    uint64_t pmcr;
    __asm__ volatile("mrs %0, pmcr_el0":"=r"(pmcr));
    pmcr=(pmcr|1u|(1u<<6))&~(2u|4u|8u);
    __asm__ volatile("msr pmccfiltr_el0, xzr\n msr pmcntenset_el0, %0\n msr pmcr_el0, %1\n isb"::"r"(UINT64_C(1)<<31),"r"(pmcr):"memory");
}
static uint64_t ticks_now(void){uint64_t v;__asm__ volatile("mrs %0, cntpct_el0":"=r"(v));return v;}
static uint64_t sctlr_now(void){uint64_t v;__asm__ volatile("mrs %0, sctlr_el1":"=r"(v));return v;}
static uint64_t midr_now(void){uint64_t v;__asm__ volatile("mrs %0, midr_el1":"=r"(v));return v;}
#else
static uint64_t cycles_now(void){return 0;}
static void cycles_enable(void){}
static uint64_t ticks_now(void){return 0;}
static uint64_t sctlr_now(void){return 0;}
static uint64_t midr_now(void){return 0;}
#endif
int os_npu_register(const void *model,size_t bytes,unsigned *entry)
{
    /* The graph is admitted like a linked one and must fit the bounds declared in the session (npu_runtime_register). */
    if(!(caller()&&entry&&npu_runtime_register(&npu.runtime,model,bytes,entry)))return 0;
    const struct npu_runtime_counters *c=&npu.runtime.counters;
    os_npu_register_record=(struct os_npu_register_record){sealed_record.graphs+c->registered,sealed_record.bytes+c->registered_bytes,
        sealed_record.copy_us+c->register_copy_us,sealed_record.decode_us+c->register_decode_us,
        sealed_record.layout_us+c->register_layout_us,{0}};
    return 1;
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
struct npu_runtime *os_npu_queue_runtime(void)
{return caller()&&npu.opened&&os_npu_queued()?&npu.runtime:NULL;}
int os_npu_queue_begin(void)
{return caller()&&npu.opened&&os_npu_queued()&&claim();}
void os_npu_queue_end(void){complete();}
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
static void guard_check(void);
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
                guard_check();atomic_store_explicit(&npu.stopped,1,memory_order_release);os_application_wake_main();
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
                atomic_store_explicit(&perf_ready,1,memory_order_release);
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
                            guard_check();atomic_store_explicit(&npu.stopped,1,memory_order_release);os_application_wake_main();
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
/* A session that does not grant the NPU leaves its entries without effect: no pin, task, interrupt or register. */
int os_npu_pins(struct a733_pin_handoff *pins){return pins&&os_session_admitted();}
/* Each start refusal records its own source line (group NPU, value 0). */
static int refused(uint32_t site,const void *here){board_fail_note(site,0,here);return 0;}
#define REFUSE() refused(SUNSTONE_CRUMB_FAIL_SITE(NPU),SUNSTONE_CRUMB_HERE())
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
int os_npu_queued(void)
{
    const struct os_session *s=os_session();
    return s&&os_granted(OS_GRANT_NPU)&&os_npu_session_queued(&s->npu);
}
/* The queue adapter's tables are carved into the object that os_npu_queue_sdk_bind binds, so the bind comes first: it runs at
 * admission with the tables, not at start. The host check of the open path runs this same function. */
int os_npu_queue_prepare(struct npu_runtime *rt)
{
    return os_npu_queue_sdk_bind()&&os_npu_queue_tables(rt);
}
/* A pool placed above 4 GiB is tested against an alias: if the NPU drops the high physical bits, its accesses land at the same low
 * 32 bits. The OS fills a kernel-owned extent at that alias with a pattern, publishes it, and checks it when the NPU client ends, so
 * a stray write is seen. An alias below DRAM (the both-bits placement) is the SoC's own address space and cannot be guarded. The
 * runner copies the record out of image RAM. */
struct os_npu_guard_record{uint64_t pool_base,pool_bytes,guard_base,guard_bytes,checked,differing_words,first_difference,pattern;};
struct os_npu_guard_record os_npu_guard_record;
#define GUARD_PATTERN UINT64_C(0xa5a5a5a5a5a5a5a5)
static struct umem_buf guard_buf;
/* Reserved before the runtime prepares, so that nothing else takes the alias; trimmed and filled once the pool is trimmed. */
static int guard_reserve(struct umem *led,uint64_t base,uint64_t bytes)
{
    const uint64_t alias=base&UINT64_C(0xffffffff);
    if(alias==base||alias<UINT64_C(0x42200000)||alias+bytes>UINT64_C(0x340000000))return 1;   /* nothing to guard */
    const struct umem_request r={.bytes=bytes,.align=4096,.base=alias,.owner=UMEM_OWNER_KERNEL,.flags=UMEM_RAW|UMEM_AT};
    return umem_alloc(led,&r,&guard_buf)==UMEM_OK;
}
static int guard_finish(struct umem *led,uint64_t base,uint64_t kept)
{
    os_npu_guard_record.pool_base=base;os_npu_guard_record.pool_bytes=kept;os_npu_guard_record.pattern=GUARD_PATTERN;
    if(!guard_buf.cell)return 1;
    if(umem_trim(led,&guard_buf,kept)!=UMEM_OK)return 0;
    const uint64_t alias=guard_buf.base;
    memset((void *)(uintptr_t)alias,0xa5,(size_t)kept);
    if(!aarch64_device_cache(AARCH64_CACHE_CLEAN_INVALIDATE,(uintptr_t)alias,(size_t)kept))return 0;
    os_npu_guard_record.guard_base=alias;os_npu_guard_record.guard_bytes=kept;
    return os_device_range_publish(alias,kept);
}
/* Called when the NPU has stopped, before the stopped proof is published: how many words of the guard changed. */
static void guard_check(void)
{
    struct os_npu_guard_record *r=&os_npu_guard_record;
    if(!r->guard_bytes||r->checked)return;
    if(!aarch64_device_cache(AARCH64_CACHE_CLEAN_INVALIDATE,(uintptr_t)r->guard_base,(size_t)r->guard_bytes))return;
    const volatile uint64_t *w=(const volatile uint64_t *)(uintptr_t)r->guard_base;
    uint64_t differing=0,first=0;
    for(uint64_t i=0;i<r->guard_bytes/8;++i)if(w[i]!=GUARD_PATTERN){if(!differing)first=r->guard_base+i*8;++differing;}
    r->differing_words=differing;r->first_difference=first;r->checked=1;
}
/* Each refusal of the admission-time preparation records its source line in group NPU, with the detail that says what did not
 * fit; os_npu_start then refuses (the application is not stopped by an NPU it cannot use). */
static int unprepared(uint32_t detail,const void *here){board_fail_note(SUNSTONE_CRUMB_FAIL_SITE(NPU),detail,here);return 1;}
#define UNPREPARED(detail) unprepared((detail),SUNSTONE_CRUMB_HERE())
const unsigned os_npu_client_model=0;
int os_npu_pool_prepare(struct umem *led,uint16_t owner)
{
    /* Not granted: no reservation, published range or guard. */
    if(!os_granted(OS_GRANT_NPU))return 1;
    const struct os_session *s=os_session_admitted();
    if(!s||!led||npu.prepared||npu.attempted)return 1;
    const struct os_npu_session *g=&s->npu;
    static const struct a733_mapping_rule wb={A733_MAPPING_WRITE,1,{{A733_MAIR_NORMAL_WB,A733_SH_INNER}},0,1};
    uint64_t total_begin=0,reserve_begin=0,reserve_end=0,prepare_end=0,finish_end=0;
    cycles_enable();
    const uint64_t cycles_begin=cycles_now(),ticks_begin=ticks_now();
    (void)clock_now(&total_begin);
    /* The application's declaration sets the slots; the reservation covers them all, so that nothing is allocated on
     * the job path. A declaration that does not fit names the resource in the record's value. */
    if(os_npu_declaration_admitted(g)!=NPU_DECLARATION_OK)return 1;
    const struct npu_declaration declared=os_npu_declaration_of(g);
    const unsigned slots=declared.slots;
    const int queued=os_npu_session_queued(g);
    /* The image links no graph. Either the session names the input region that holds the application's graphs
     * (graph_region): admission copies each one out of it and sizes the pool from the copies, and a later registration is
     * refused (NPU_REGISTER_SEALED); or the application registers every graph at run time (ss_npu_register_graph), within the
     * bounds the session declares, and each slot is sized by those bounds. */
    struct npu_runtime_bounds bounds={0};
    struct npu_runtime_model *models=NULL;
    unsigned model_count=0;
    struct npu_meta *const meta=meta_init(led);
    if(g->graph_region){
        if(!queued)return UNPREPARED(0x816);    /* the synchronous service reloads one slot with different graphs: no slot is sized by one graph */
        const struct ss_manifest *const m=ss_application.manifest;
        if(!m||g->graph_region>m->region_count||m->regions[g->graph_region-1].kind!=SS_REGION_INPUT)return UNPREPARED(0x817);
        const struct ss_region *const region=&m->regions[g->graph_region-1];
        const uint8_t *const source=(const uint8_t *)(uintptr_t)region->address;
        if(os_npu_index_check(source,region->bytes,declared.graphs,declared.graph_bytes)!=OS_NPU_INDEX_OK||
           declared.slots!=declared.graphs)return UNPREPARED(0x818);
        model_count=declared.graphs;
        /* Nothing reads the application's region again after this copy: the runtime decodes and lays out the copies. */
        uint64_t t0,t1;
        if(!clock_now(&t0))return UNPREPARED(0x819);
        models=npu_meta_allocate(meta,(size_t)model_count*sizeof(*models));
        if(!models)return UNPREPARED(0x81a);
        uint64_t total=0;
        for(unsigned i=0;i<model_count;++i){
            uint64_t offset,length;
            os_npu_index_entry(source,i,&offset,&length);
            void *const copy=npu_meta_allocate(meta,(size_t)length);
            if(!copy)return UNPREPARED(0x81a);
            memcpy(copy,source+offset,(size_t)length);
            models[i]=(struct npu_runtime_model){copy,(size_t)length};
            total+=length;
        }
        if(!clock_now(&t1))return UNPREPARED(0x819);
        sealed_record.graphs=model_count;sealed_record.bytes=total;sealed_record.copy_us=t1-t0;
        bounds.per_graph=1;
    }else{
        if(!declared.graph_bytes||!declared.tensor_bytes)return UNPREPARED(0x808);
        bounds=(struct npu_runtime_bounds){.capacity=declared.graphs,.graph_bytes=declared.graph_bytes,.tensor_bytes=declared.tensor_bytes};
    }
    /* The ring keeps graph i resident in slot i: each slot is sized by its own graph, not by the largest. The queue's
     * tables (handles, tensors, edges) are reserved with the rest, from the declared links. */
    if(queued){bounds.queue=1;bounds.queue_links=declared.links;bounds.queue_image_links=(unsigned)ss_npu_queue_edges.count;}
    /* Reserve the largest extent the NPU can address inside one 4 GiB window (its tables share one physical window and its
     * working address limit is the device-width table's), let prepare place the declaration from the start of it, then give
     * back what it did not use. Sizing the reservation first would admit every graph twice before the scheduler, which the
     * secondary cores wait for one second at most: a layout takes tens of milliseconds on a Cortex-A55 at the boot clock. */
    uint64_t base,bytes;
    (void)clock_now(&reserve_begin);
    const uint64_t request_base=g->pool_request[0],request_bytes=g->pool_request[1];
    const int requested=request_base||request_bytes;
    if(requested){
        /* A placement request: exactly this extent, or a refusal. The ledger refuses it unless it is free DRAM above the image,
         * and refuses one that crosses a 4 GiB window. The NPU's working limit in the device-width table is raised to its source
         * width for this boot, so an extent above 4 GiB is allowed; the table itself keeps 4 GiB. */
        if(!request_base||!request_bytes||((request_base|request_bytes)&(NPU_RUNTIME_GROUP_BYTES-1)))return UNPREPARED(0x810);
        if(umem_device_limit_set(led,0,UINT64_C(1)<<40)!=UMEM_OK)return UNPREPARED(0x811);
        base=request_base;bytes=request_bytes;
    }else{
        const struct umem_request shape={.policy=UMEM_WB,.device_mask=UMEM_DEV_NPU,.window=UINT64_C(1)<<32};
        if(umem_largest(led,&shape,&base,&bytes)!=UMEM_OK)return UNPREPARED(0x801);
        bytes&=~(uint64_t)(NPU_RUNTIME_GROUP_BYTES-1);
        if(!bytes)return UNPREPARED(0x802);
    }
    const struct umem_request take={.bytes=bytes,.align=NPU_RUNTIME_GROUP_BYTES,.base=base,.owner=owner,.flags=UMEM_RAW|UMEM_AT,
        .access=UMEM_PROT_READ|UMEM_PROT_WRITE,.device_mask=UMEM_DEV_NPU,.window=UINT64_C(1)<<32};
    if(umem_alloc(led,&take,&npu.pool)!=UMEM_OK)return UNPREPARED(requested?0x812:0x803);
    if(requested&&!guard_reserve(led,base,bytes))return UNPREPARED(0x814);
    (void)clock_now(&reserve_end);
    struct npu_runtime_config config={.models=models,.count=model_count,.slots=slots,.bounds=bounds,.cpu=(uintptr_t)base,
        .physical=base,.bytes=bytes,.register_cpu=0x03600000,
        .register_physical=0x03600000,.fatal=fatal,.meta=meta,.now_us=prepare_clock};
    uint64_t layout_begin=0,layout_end=0;
    (void)clock_now(&layout_begin);
    const int layout_ok=npu_runtime_prepare(&npu.runtime,&config);
    (void)clock_now(&layout_end);
    sealed_record.layout_us=layout_end>=layout_begin?layout_end-layout_begin:0;
    prepare_end=layout_end;
    if(!layout_ok){
        /* Name the cause: prepare says which resource did not fit (a graph that is refused, the NPU's address space, or the
         * memory for the declaration's tables and regions). The extent stays held. */
        return UNPREPARED(npu.runtime.refusal!=NPU_DECLARATION_OK?(uint32_t)npu.runtime.refusal:0x804);
    }
    /* The bounds are an upper limit on what the graphs need, never a size: a graph beyond them is refused. */
    for(unsigned i=0;i<model_count&&declared.tensor_bytes;++i){
        const struct nbg_assembly_layout *p=&npu.runtime.plans[i];
        size_t in=0,out=0;
        for(unsigned j=0;j<p->inputs;++j)in+=p->buffers[NBG_ALLOC_INPUT+j].payload_bytes;
        for(unsigned j=0;j<p->outputs;++j)out+=p->buffers[NBG_ALLOC_INPUT+p->inputs+j].payload_bytes;
        if(in>declared.tensor_bytes||out>declared.tensor_bytes)return UNPREPARED(0x81b);
    }
    const size_t kept=npu_runtime_trim_parent(&npu.runtime);
    if(!kept||umem_trim(led,&npu.pool,kept)!=UMEM_OK)return UNPREPARED(0x805);
    if(!a733_mapping_check_stride(aarch64_device_translate,(uintptr_t)base,base,kept,0x200000,&wb))return UNPREPARED(0x806);
    /* The reservation is visible to the NPU from here: published before anything can use it. */
    if(!os_device_range_publish(base,kept))return UNPREPARED(0x807);
    if(requested&&!guard_finish(led,base,kept))return UNPREPARED(0x813);
    /* The queue's tables are in the reservation: take them before the scheduler starts, so the application's first open
     * finds its ring sized for the declaration. */
    if(queued&&!os_npu_queue_prepare(&npu.runtime))return UNPREPARED((uint32_t)NPU_DECLARATION_MEMORY);
    os_npu_register_record=sealed_record;
    (void)clock_now(&finish_end);
    const struct npu_runtime_prepare_parts *pt=&npu.runtime.parts;
    os_npu_prepare_record=(struct os_npu_prepare_record){sealed_record.graphs,sealed_record.bytes,sealed_record.copy_us,
        reserve_end-reserve_begin,pt->decode_us,pt->layout_us,pt->carve_us,pt->place_us,pt->fill_us,pt->native_us,
        finish_end-prepare_end,finish_end-total_begin,cycles_now()-cycles_begin,ticks_now()-ticks_begin,sctlr_now(),midr_now()};
    npu.prepared=1;
    return 1;
}
int os_npu_start(void)
{
    extern int os_npu_sdk_bind(const struct npu_runtime *);
    extern int os_npu_diagnostic_bind(struct npu_runtime *) __attribute__((weak));
    /* Not granted: no binding, task, interrupt or register access. */
    if(!os_granted(OS_GRANT_NPU))return 1;
    if(!ss_npu_service_bind_event(sdk_notify,NULL))return REFUSE();
    const struct os_session *s=os_session_admitted();
    if(!s)return REFUSE();
    if(npu.attempted)return REFUSE();
    if(board.session.deadline_us<=10000)return REFUSE();
    npu.attempted=1;
    const struct os_npu_session *g=&s->npu;
    /* The reservation was made when the application was admitted (os_npu_pool_prepare). */
    if(!npu.prepared)return REFUSE();
    if(!os_npu_sdk_bind(&npu.runtime))return REFUSE();
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
    /* The low boot mode of the grant's performance service: the NPU starts at its 492 MHz point. */
    if(os_perf_enabled()&&os_perf_boot_mode()==2&&!a733_npu_startup_rate(&npu.startup,492))return REFUSE();
    creating=1;
    npu.bootstrap=xTaskCreateStaticAffinitySet(bootstrap,"os-npu-boot",STACK,NULL,4,npu.stacks[0],&npu.tasks[0],2);
    npu.client=xTaskCreateStaticAffinitySet(os_npu_client_task,"os-npu-app",STACK,NULL,3,npu.stacks[1],&npu.tasks[1],2);
    creating=0;
    /* Under the ring the graphs are loaded when it opens, after the scheduler has started (npu_runtime_load_resident):
     * hashing and assembling megabytes would outlast the second that the secondary cores wait for the primary core. */
    TaskHandle_t clients[]={npu.client};
    uint64_t now;
    if(!npu.bootstrap||!npu.client)return REFUSE();
    if(!a733_npu_startup_bind_task(&npu.startup,npu.bootstrap))return REFUSE();
    if(!(os_npu_session_queued(g)?npu_runtime_bind_resident(&npu.runtime,&npu.startup,npu.client,4,2):
         npu_runtime_bind(&npu.runtime,&npu.startup,clients,4,2)))return REFUSE();
    if(!os_npu_irq_bind(&npu.runtime))return REFUSE();
    if(os_npu_diagnostic_bind&&!os_npu_diagnostic_bind(&npu.runtime))return REFUSE();
    if(npu.end>(UINT64_MAX>>3)-5000000||!clock_now(&now))return REFUSE();
    if(now>=npu.end)return REFUSE();
    npu.initial_deadline=now+1000000;
    atomic_init(&npu.gate,npu.initial_deadline<<3);npu.started=1;return 1;
}
