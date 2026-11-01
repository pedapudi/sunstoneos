/* Instruction-model workload using the actual cached SMP port, kernel locks,
 * task notification paths, IRQ vector and migration. Never a board image. */
#include "FreeRTOS.h"
#include "task.h"
#include "platform.h"
#include "sunstone_deadline.h"
#include <stdatomic.h>
static StaticTask_t task_state[16];
static StackType_t task_stack[16][2048];
static TaskHandle_t owners[16];
static atomic_uint ready,complete;
static atomic_uint_fast64_t alarm_at;
static uint64_t startup_limit;
static void wait_alarm(uint64_t until)
{
    int result;
    do {result=sunstone_deadline_wait_until(until);configASSERT(result!=SUNSTONE_DEADLINE_ERROR);}
    while(result==SUNSTONE_DEADLINE_NOTIFIED);
    configASSERT(sunstone_deadline_now_us()>=until);
}
static void worker(void *arg)
{
    unsigned original=(uintptr_t)arg;
    configASSERT(portGET_CORE_ID()==original%8&&sunstone_deadline_register());
    atomic_fetch_add_explicit(&ready,1,memory_order_release);
    xTaskNotifyGive(owners[0]);
    if(!original){
        while(atomic_load_explicit(&ready,memory_order_acquire)!=16){
            configASSERT(sunstone_deadline_wait_until(startup_limit)!=SUNSTONE_DEADLINE_ERROR);
            configASSERT(sunstone_deadline_now_us()<startup_limit);
        }
        atomic_store_explicit(&alarm_at,sunstone_deadline_now_us()+10000,memory_order_release);
        for(unsigned i=1;i<16;++i)xTaskNotifyGive(owners[i]);
    }else while(!atomic_load_explicit(&alarm_at,memory_order_acquire)){
        configASSERT(sunstone_deadline_wait_until(startup_limit)!=SUNSTONE_DEADLINE_ERROR);
        configASSERT(sunstone_deadline_now_us()<startup_limit);
    }
    /* All sixteen admitted slots share one expiry. The PPI wakes one owner per
     * entry, and the actual ISR notification may preempt a different core. */
    wait_alarm(atomic_load_explicit(&alarm_at,memory_order_acquire));
    uint64_t future=sunstone_deadline_now_us()+50000;
    xTaskNotifyGive(xTaskGetCurrentTaskHandle());
    configASSERT(sunstone_deadline_wait_until(future)==SUNSTONE_DEADLINE_NOTIFIED);
    configASSERT(sunstone_deadline_now_us()<future);
    /* Move the same task handle to another core before the next arm. */
    unsigned next=(original+1)%8;
    vTaskCoreAffinitySet(NULL,1u<<next);
    configASSERT(portGET_CORE_ID()==next);
    wait_alarm(sunstone_deadline_now_us()+2000);
    atomic_fetch_or_explicit(&complete,1u<<original,memory_order_release);
    if(original){xTaskNotifyGive(owners[0]);vTaskSuspend(NULL);}
    while(atomic_load_explicit(&complete,memory_order_acquire)!=65535){
        configASSERT(sunstone_deadline_wait_until(startup_limit)!=SUNSTONE_DEADLINE_ERROR);
        configASSERT(sunstone_deadline_now_us()<startup_limit);
    }
    vTaskCoreAffinitySet(NULL,1);
    configASSERT(portGET_CORE_ID()==0);
    platform_exit(0);
}
void vApplicationStackOverflowHook(TaskHandle_t task,char *name)
{(void)task;platform_panic(name,2);}
void a733_smp_diagnostic_start(void)
{
    startup_limit=sunstone_deadline_now_us()+500000;
    for(unsigned i=0;i<16;++i){
        owners[i]=xTaskCreateStaticAffinitySet(worker,"deadline",2048,(void *)(uintptr_t)i,3,
            task_stack[i],&task_state[i],1u<<(i%8));
        configASSERT(owners[i]);
    }
    vTaskStartScheduler();platform_panic("scheduler returned",3);
}
