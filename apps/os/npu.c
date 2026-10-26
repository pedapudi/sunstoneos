/* Optional OS catalogue runtime. The board owns every power/clock provider;
 * this consumer owns one fixed RAM reservation, startup row and client slot. */
#include "os.h"
#include "npu.h"
#include "../board/application.h"
#include "a733_mapping_check.h"
#include "aarch64_device_memory.h"
#include "device_memory_pool.h"
#include "boot.h"
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
} npu;
static unsigned creating; /* Only while os_npu_start creates its two tasks. */
extern unsigned char __os_npu_pool_start[],__os_npu_pool_end[];
static int clock_now(uint64_t *now)
{return a733_counter_clock_now(&providers.shared.counter,now);}
static int fail(void)
{atomic_store_explicit(&npu.failed,1,memory_order_release);board_fail(NULL);return -1;}
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
    /* A freshness update alone wakes no task; the waits are bounded. */
    if(!((old^next)&(STOP|ALLOW)))return;
    if(stop&&npu.client)xTaskNotifyGive(npu.client);
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
static int watch(uint64_t *now)
{
    if(!live()||!clock_now(now)||*now>=npu.end)return 0;
    uint64_t word=atomic_load_explicit(&npu.gate,memory_order_acquire),until=word>>3;
    if(!(word&STOP)&&*now>=until&&*now-until>=4000000) {
        os_npu_thermal(0,1,0,*now);
        word=atomic_load_explicit(&npu.gate,memory_order_acquire);
    }
    return !(word&STOP)||*now<word>>3;
}
int os_npu_stopping(void)
{return !live()||(atomic_load_explicit(&npu.gate,memory_order_acquire)&STOP)!=0;}
unsigned os_npu_stopped(void){return atomic_load_explicit(&npu.stopped,memory_order_acquire);}
#ifdef SUNSTONE_A733_OS_PERF
/* Performance modes: the request number and rate in one word, the answer
 * number and result in another; the bootstrap serves requests only after the
 * local stop. */
static atomic_uint perf_request,perf_answer,perf_ready;
int os_npu_perf_ready(void)
{return atomic_load_explicit(&perf_ready,memory_order_acquire) && !atomic_load_explicit(&npu.failed,memory_order_acquire);}
int os_npu_perf_stopped(void){return os_npu_perf_ready() && os_npu_stopped();}
int os_npu_perf_retune(unsigned mhz,unsigned sequence)
{
    if(!npu.bootstrap || (mhz!=492 && mhz!=852) || !sequence || sequence>0xffff)return 0;
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
        const unsigned request=atomic_exchange_explicit(&perf_request,0,memory_order_acq_rel);
        if(!request)continue;
        const int r=a733_npu_startup_retune(&npu.startup,request&0xffff);
        atomic_store_explicit(&perf_answer,(request>>16)<<2|(r==1 ? 1u : r==0 ? 2u : 3u),memory_order_release);
        if(r<0){(void)fail();vTaskSuspend(NULL);}
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
    uint64_t now,ticket;
    struct npu_service_reply reply;
    if(!watch(&now))return fail();
    uint64_t end=npu.end,word=atomic_load_explicit(&npu.gate,memory_order_acquire);
    if((word&STOP)&&(word>>3)<end)end=word>>3;
    if(end<=now)return fail();
    TickType_t wait=pdMS_TO_TICKS(2000);
    const uint64_t ticks=(end-now)*configTICK_RATE_HZ/1000000;
    if(ticks<wait)wait=(TickType_t)ticks;
    if(!wait||!npu_runtime_send(&npu.runtime,0,op,model,timeout,wait,&ticket)||
       !npu_runtime_collect(&npu.runtime,0,ticket,&reply,wait)||!watch(&now)||
       reply.ticket!=ticket||reply.result!=NPU_SERVICE_OK||reply.fault!=A733_NPU_NO_FAULT||
       reply.state!=(op==NPU_SERVICE_CLOSE?A733_NPU_CLOSED:op==NPU_SERVICE_SUBMIT?A733_NPU_DONE:A733_NPU_READY))return fail();
    return 1;
}
int os_npu_open(void)
{
    if(!caller()||npu.opened)return 0;
    if(!atomic_load_explicit(&npu.active,memory_order_acquire))return 0;
    if(call(NPU_SERVICE_OPEN,0,1000000)!=1)return -1;
    npu.opened=1;return 1;
}
int os_npu_load(unsigned entry)
{
    if(!caller()||!npu.opened||entry>=OS_NPU_MODELS||os_npu_stopping())return 0;
    return call(NPU_SERVICE_LOAD,entry,0);
}
int os_npu_tensor(enum nbg_section_id section,unsigned index,struct npu_runtime_tensor *tensor)
{
    if(!caller()||!npu.opened)return 0;
    return npu_runtime_tensor(&npu.runtime,0,section,index,tensor);
}
int os_npu_submit(uint64_t timeout)
{
    const struct os_session *s=os_session();
    if(!caller()||!npu.opened||!timeout||timeout>1000000||!s||npu.jobs>=s->npu.jobs||!claim())return 0;
    int result=call(NPU_SERVICE_SUBMIT,0,timeout);
    complete();if(result==1)++npu.jobs;return result;
}
int os_npu_close(void)
{
    if(!caller()||!npu.opened||atomic_load_explicit(&npu.closed,memory_order_acquire))return 0;
    uint64_t now;if(!clock_now(&now))return fail();
    os_npu_thermal(0,1,0,now);
    if(call(NPU_SERVICE_CLOSE,0,0)!=1)return -1;
    npu.opened=0;atomic_store_explicit(&npu.closed,1,memory_order_release);
    xTaskNotifyGive(npu.bootstrap);return 1;
}
static void bootstrap(void *unused)
{
    (void)unused;unsigned begun=0;
    for(;;) {
        uint64_t now;if(!watch(&now))break;
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
        if(r==A733_NPU_PLATFORM_RETAINED)break;
        if(r==A733_NPU_PLATFORM_READY) {
            enum npu_service_result activated=npu_service_activate(&npu.runtime.service);
            if(activated==NPU_SERVICE_OK) {
#ifdef SUNSTONE_A733_OS_PERF
                atomic_store_explicit(&perf_ready,1,memory_order_release);
#endif
                atomic_store_explicit(&npu.active,1,memory_order_release);xTaskNotifyGive(npu.client);
                for(;;) {
                    if(!watch(&now))goto failed;
                    if(atomic_load_explicit(&npu.closed,memory_order_acquire)) {
                        r=npu_runtime_stop_poll(&npu.runtime);
                        if(r==A733_NPU_PLATFORM_RETAINED)goto failed;
                        if(r==A733_NPU_PLATFORM_READY) {
                            atomic_store_explicit(&npu.stopped,1,memory_order_release);
                            xTaskNotifyGive(npu.client);STOPPED_WAIT();return;
                        }
                        vTaskDelay(1);
                    } else (void)ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(25));
                }
            }
            if(activated!=NPU_SERVICE_BUSY)break;
        }
        /* A PENDING startup blocks until its power-service notification, its
         * deadline or the last tick of its settle; a thermal notification
         * ends the block early. */
        (void)ulTaskNotifyTake(pdTRUE,r==A733_NPU_PLATFORM_PENDING ? a733_npu_startup_wait_ticks(&npu.startup) : 1);
    }
failed:
    (void)fail();vTaskSuspend(NULL);
}
/* The bootstrap and the registered client call a733_counter_clock_now, which
 * masks DAIF, so both start at EL1t. Only their two creations in
 * os_npu_start qualify; any other task with these entries starts at EL0. */
int os_npu_task_privileged(void (*entry)(void *),void *context)
{return (creating&&!context&&(entry==bootstrap||entry==os_npu_client_task))||
    npu_service_task_privileged(entry,context)==pdTRUE;}
int os_npu_pins(struct a733_pin_handoff *pins){return pins&&os_session_admitted();}
int os_npu_start(void)
{
    const struct os_session *s=os_session_admitted();
    if(!s||npu.attempted||board.session.deadline_us<=10000)return 0;
    npu.attempted=1;
    const struct os_npu_session *g=&s->npu;
    static const struct a733_mapping_rule wb={A733_MAPPING_WRITE,1,{{A733_MAIR_NORMAL_WB,A733_SH_INNER}},0,1};
    uintptr_t start=(uintptr_t)__os_npu_pool_start,end=(uintptr_t)__os_npu_pool_end;
    /* The reservation fits any one catalogue entry; the runtime takes the
     * linked entry's exact requirement from its start. */
    const size_t need=npu_runtime_requirements(os_npu_models,OS_NPU_MODELS,1);
    if(start!=OS_NPU_POOL_START||end-start!=OS_NPU_POOL_BYTES||!need||need>OS_NPU_POOL_BYTES||
       !a733_mapping_check_stride(aarch64_device_translate,start,start,end-start,0x200000,&wb)||
       !device_memory_pool_init_typed(&npu.pool,start,start,end-start,DEVICE_MEMORY_CLASS_DMA32,DEVICE_MEMORY_WRITE_BACK)||
       !device_memory_acquire(&npu.pool,g->owner,need,&npu.lease,&npu.extent)||
       npu.extent.cpu!=start||npu.extent.physical!=start||npu.extent.bytes!=need)return 0;
    struct npu_runtime_config config={.count=OS_NPU_MODELS,.slots=1,.cpu=npu.extent.cpu,
        .physical=npu.extent.physical,.bytes=npu.extent.bytes,.register_cpu=0x03600000,
        .register_physical=0x03600000,.fatal=fatal};
    memcpy(config.models,os_npu_models,sizeof(os_npu_models));
    if(!npu_runtime_prepare(&npu.runtime,&config)){device_memory_retain(&npu.pool,npu.lease);return 0;}
    device_memory_lookup(&npu.pool,npu.lease)->state=DEVICE_MEMORY_DEVICE;
    const struct a733_board_session *b=&s->board;
    npu.end=board.session.deadline_us-10000;
    const struct a733_npu_startup_config startup={{{b->fabric[0],b->fabric[1],b->fabric[2]},
        {b->fabric_hz[0],b->fabric_hz[1],b->fabric_hz[2]}},g->owner,npu.end,g->pck_client};
    const struct a733_npu_startup_providers p={&providers.shared.counter,&providers.shared.clocks,
        &providers.shared.locks,&providers.shared.gpu,&providers.shared.rails,&providers.domains};
    if(!a733_npu_startup_init(&npu.startup,&startup,&p,NULL,NULL))return 0;
#if defined(SUNSTONE_A733_OS_PERF) && OS_PERF_BOOT_MODE==2
    /* The low boot mode: the NPU starts at its 492 MHz point. */
    if(!a733_npu_startup_rate(&npu.startup,492))return 0;
#endif
    creating=1;
    npu.bootstrap=xTaskCreateStaticAffinitySet(bootstrap,"os-npu-boot",STACK,NULL,4,npu.stacks[0],&npu.tasks[0],2);
    npu.client=xTaskCreateStaticAffinitySet(os_npu_client_task,"os-npu-app",STACK,NULL,3,npu.stacks[1],&npu.tasks[1],2);
    creating=0;
    TaskHandle_t clients[]={npu.client};
    uint64_t now;
    if(!npu.bootstrap||!npu.client||!a733_npu_startup_bind_task(&npu.startup,npu.bootstrap)||
       !npu_runtime_bind(&npu.runtime,&npu.startup,clients,4,2)||!os_npu_irq_bind(&npu.runtime)||
       npu.end>(UINT64_MAX>>3)-5000000||!clock_now(&now)||now>=npu.end)return 0;
    npu.initial_deadline=now+1000000;
    atomic_init(&npu.gate,npu.initial_deadline<<3);npu.started=1;return 1;
}
