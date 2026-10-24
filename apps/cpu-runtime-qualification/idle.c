#include "FreeRTOS.h"
#include "task.h"
#include "platform.h"
#include "cached.h"
#include "runtime.h"

/* All storage is inside the final cached writable clean. Each CPU writes
 * only its own counters/flags; CPU0 alone publishes the phase deadline. */
static TaskHandle_t backgrounds[QUAL_CORES],workers[QUAL_CORES];
static uint64_t ready[QUAL_CORES],phase_deadline;
uint64_t a733_qualification_idle_completed[QUAL_CORES];
uint64_t a733_qualification_idle_wakes[QUAL_CORES];
static uint64_t sgi_before[QUAL_CORES];

void a733_qualification_background(unsigned core,void *task)
{
    configASSERT(core<QUAL_CORES && task && !backgrounds[core]);
    backgrounds[core]=task;
}

void a733_qualification_idle(void)
{
    unsigned core=portGET_CORE_ID();
    configASSERT(core<QUAL_CORES);
    /* Save eligibility in this idle invocation's context. An older call
     * cannot publish merely because a worker became ready while it slept. */
    uint64_t epoch=__atomic_load_n(&ready[core],__ATOMIC_ACQUIRE);
    a733_cpu_idle();
    if(epoch && epoch==a733_cached_runtime[core].epoch){
        configASSERT(portGET_CORE_ID()==core);
        __atomic_add_fetch(&a733_qualification_idle_completed[core],1,__ATOMIC_RELEASE);
    }
}

static void before(uint64_t deadline)
{configASSERT(deadline && a733_smp_counter()<deadline);}

void a733_qualification_idle_stage(unsigned core)
{
    configASSERT(core<QUAL_CORES && portGET_CORE_ID()==core && backgrounds[core]);
    uint64_t epoch=a733_cached_runtime[core].epoch;
    configASSERT(epoch && !__atomic_load_n(&ready[core],__ATOMIC_ACQUIRE));
    /* The background is pinned to this core, whose higher-priority worker
     * is running now. It cannot be executing elsewhere when suspended. */
    vTaskSuspend(backgrounds[core]);
    workers[core]=xTaskGetCurrentTaskHandle();
    sgi_before[core]=__atomic_load_n(&a733_smp_core_results[core].sgis,__ATOMIC_ACQUIRE);
    if(!core){
        uint64_t end=a733_smp_counter()+UINT64_C(16000)*24;
        if(end>a733_cached_runtime[0].diagnostic_deadline)end=a733_cached_runtime[0].diagnostic_deadline;
        __atomic_store_n(&phase_deadline,end,__ATOMIC_RELEASE);
    }
    __atomic_store_n(&ready[core],epoch,__ATOMIC_RELEASE);
    uint64_t end;
    if(!core){
        end=__atomic_load_n(&phase_deadline,__ATOMIC_ACQUIRE);
        for(unsigned c=1;c<QUAL_CORES;++c)
            while(__atomic_load_n(&ready[c],__ATOMIC_ACQUIRE)!=epoch){
                before(end);vTaskDelay(1);before(end);
            }
        /* A notification is counted even if it precedes the target's wait.
         * Do not wait for idle completion before providing this wake. */
        for(unsigned c=1;c<QUAL_CORES;++c){before(end);xTaskNotifyGive(workers[c]);}
    }else{
        /* Another core can still be finishing its 32 rounds. This initial
         * wait uses the remaining session bound; CPU0 starts the separate
         * 16 ms phase only when its own worker reaches this stage. */
        uint64_t now=a733_smp_counter(),limit=a733_cached_runtime[core].diagnostic_deadline;
        configASSERT(now<limit);
        TickType_t ticks=(TickType_t)((limit-now+23999)/24000);
        configASSERT(ticks && ulTaskNotifyTake(pdTRUE,ticks)==1);
        end=__atomic_load_n(&phase_deadline,__ATOMIC_ACQUIRE);
        before(end);configASSERT(portGET_CORE_ID()==core);
        a733_qualification_idle_wakes[core]=1;
    }
    /* An IRQ can preempt the idle hook before or immediately after WFI.
     * Two separate blocked intervals let that saved context execute WFI,
     * then resume its completion store. Future tick wakes prevent a cycle
     * in which the owner withholds the very interrupt idle needs. */
    for(unsigned i=0;i<2;++i){
        before(end);vTaskDelay(2);before(end);
        configASSERT(portGET_CORE_ID()==core);
        a733_qualification_idle_wakes[core]|=UINT64_C(2)<<i;
    }
    configASSERT(__atomic_load_n(&a733_qualification_idle_completed[core],__ATOMIC_ACQUIRE));
    if(core)configASSERT(__atomic_load_n(&a733_smp_core_results[core].sgis,__ATOMIC_ACQUIRE)>sgi_before[core]);
}
