/* Bounded extension of the finite FP workload through the OS application kit.
 * Startup, clocks, thermal supervision and terminal cleanup are OS services. */
#include "sunstone_app.h"
#include "FreeRTOS.h"
#include "task.h"
#include "reliability.h"
#include <stdatomic.h>

struct slot {
    _Alignas(64) atomic_uint published;
    _Alignas(64) atomic_uint consumed;
    _Alignas(64) uint64_t payload[REL_WORDS];
};
static struct slot slots[REL_CORES];
static StaticTask_t tasks[REL_CORES];
static StackType_t stacks[REL_CORES][1024];
static TaskHandle_t handles[REL_CORES];
static atomic_uint started,done[REL_CORES];
static struct reliability_result result;
extern uint64_t check_fp_context(uint64_t);
static TickType_t admitted_tick;

/* Detail high nibble names the stage; low byte names the CPU. The existing
 * application failure recorder preserves both it and the caller in RTC. */
static void require(int valid,unsigned stage,unsigned core)
{if(!valid)ss_fail((stage<<8)|core);}
static void live(unsigned core)
{
    require(ss_live()>=0 && (TickType_t)(xTaskGetTickCount()-admitted_tick)<pdMS_TO_TICKS(12000),1,core);
    require(!atomic_load_explicit(&started,memory_order_acquire)||!ss_throttled(),9,core);
    ss_progress();
}
static void wait_for(atomic_uint *word,unsigned expected,unsigned core)
{
    while(atomic_load_explicit(word,memory_order_acquire)!=expected){
        live(core);
        /* A notification can precede this wait, or combine data and ack
         * events. The atomic predicate is authoritative in both cases. */
        ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(10));
        ++result.cpu[core].waits;
    }
}
static void exercise(unsigned core)
{
    unsigned previous=(core+REL_CORES-1)%REL_CORES,next=(core+1)%REL_CORES;
    require(portGET_CORE_ID()==core,2,core);
    wait_for(&started,1,core);
    for(unsigned round=1;round<=REL_ROUNDS;++round){
        require(portGET_CORE_ID()==core,2,core);
        /* A consumer's acknowledgment precedes reuse of its producer's slot.
         * Ordinary payload loads/stores have one writer and one reader. */
        wait_for(&slots[core].consumed,round-1,core);
        for(unsigned i=0;i<REL_WORDS;++i)
            slots[core].payload[i]=reliability_word(core,round,i);
        atomic_store_explicit(&slots[core].published,round,memory_order_release);
        xTaskNotifyGive(handles[next]);
        require(check_fp_context(UINT64_C(0x1234567887650000)+core*REL_ROUNDS+round),3,core);
        wait_for(&slots[previous].published,round,core);
        for(unsigned i=0;i<REL_WORDS;++i){
            uint64_t value=slots[previous].payload[i];
            require(value==reliability_word(previous,round,i),4,core);
            result.cpu[core].digest+=value;
        }
        atomic_store_explicit(&slots[previous].consumed,round,memory_order_release);
        xTaskNotifyGive(handles[previous]);
        result.cpu[core].rounds=round;
        live(core);
    }
    wait_for(&slots[core].consumed,REL_ROUNDS,core);
}
static void worker(void *argument)
{
    unsigned core=(unsigned)(uintptr_t)argument;
    exercise(core);
    /* No payload/result stores follow this release. Remaining kernel stores
     * retire at the OS per-core terminal barrier before cache cleanup. */
    atomic_store_explicit(&done[core],1,memory_order_release);
    xTaskNotifyGive(handles[ss_grant()->main_core]);
    vTaskSuspend(NULL);
    ss_fail(0x500|core);
}
void cpu_reliability_init(void)
{
    const struct ss_grant *grant=ss_grant();
    admitted_tick=xTaskGetTickCount();
    require(grant && grant->main_core==6 && grant->worker_cores==0xbf,6,0);
    for(unsigned c=0;c<REL_CORES;++c)if(c!=grant->main_core){
        handles[c]=xTaskCreateStaticAffinitySet(worker,"cpu-rel",1024,(void *)(uintptr_t)c,
            2,stacks[c],&tasks[c],(UBaseType_t)1<<c);
        require(handles[c]!=NULL,6,c);
    }
}
static int at_top(void)
{
    char clock[256];
    const char prefix[]="CLOCK cpu_l_mhz=1794 cpu_b_mhz=2002 cpu_l_mv=1000 cpu_b_mv=1050 ";
    if(ss_clock_info(clock,sizeof(clock))<(int)(sizeof(prefix)-1))return 0;
    for(unsigned i=0;i<sizeof(prefix)-1;++i)if(clock[i]!=prefix[i])return 0;
    return 1;
}
void cpu_reliability_main(const struct ss_grant *grant)
{
    unsigned core=grant->main_core;
    handles[core]=xTaskGetCurrentTaskHandle();
    while(!ss_clock_ready()){live(core);vTaskDelay(1);}
    /* Pinned 26 MHz, bin-4 vendor points from a733_cpu_clock.c. The OS
     * thermal owner can still lower them; throttling ends this diagnostic. */
    require(at_top(),7,core);
    TickType_t begin=xTaskGetTickCount();
    ss_activity_begin();
    atomic_store_explicit(&started,1,memory_order_release);
    for(unsigned c=0;c<REL_CORES;++c)if(c!=core)xTaskNotifyGive(handles[c]);
    exercise(core);
    for(unsigned c=0;c<REL_CORES;++c)if(c!=core)wait_for(&done[c],1,core);
    require(at_top(),7,core);
    ss_activity_end(1);
    result.magic=REL_MAGIC;result.rounds=REL_ROUNDS;result.core_mask=0xff;
    result.elapsed_ticks=(TickType_t)(xTaskGetTickCount()-begin);
    require(result.elapsed_ticks>=REL_ROUNDS,8,core);
    ss_finish(&result,sizeof(result));
}
/* This diagnostic has no stdout stream. Newlib's formatter pulls its syscall
 * table into the image; reject an unexpected write through its console hook.
 * The generic optional-console fix is supplied separately for integration. */
void platform_puts(const char *text)
{(void)text;ss_fail(0xa00|portGET_CORE_ID());}
