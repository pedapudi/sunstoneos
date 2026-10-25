#include "FreeRTOS.h"
#include "task.h"
#include "platform.h"
#include "boot.h"
#ifdef SUNSTONE_A733_PERF_SESSION
#include "perf_session.h"
#endif
static StaticTask_t tasks[16];
static StackType_t stacks[16][2048];
extern uint64_t check_fp_context(uint64_t);
static void background(void *argument)
{
    unsigned c=(uintptr_t)argument;
    for(;;) {
        configASSERT(portGET_CORE_ID()==c);
        __atomic_add_fetch(&a733_smp_core_results[c].background_progress,1,__ATOMIC_RELAXED);
    }
}
static void worker(void *argument)
{
    unsigned c=(uintptr_t)argument;
    for(unsigned round=0;round<32;++round) {
        configASSERT(portGET_CORE_ID()==c);
        configASSERT(check_fp_context(UINT64_C(0x1234567887650000)+c*256+round));
        __atomic_store_n(&a733_smp_core_results[c].task_rounds,round+1,__ATOMIC_RELEASE);
    }
    if(c) {vTaskSuspend(NULL);platform_panic("worker resumed",1);}
    for(unsigned i=0;i<8;++i) {
        while(__atomic_load_n(&a733_smp_core_results[i].task_rounds,__ATOMIC_ACQUIRE)!=32) {
            configASSERT(!__atomic_load_n(&a733_smp_core_results[i].failure,__ATOMIC_ACQUIRE));
            vTaskDelay(1);
        }
        configASSERT(__atomic_load_n(&a733_smp_core_results[i].background_progress,__ATOMIC_RELAXED));
        configASSERT(__atomic_load_n(&a733_smp_core_results[i].sgis,__ATOMIC_RELAXED));
        configASSERT(!__atomic_load_n(&a733_smp_core_results[i].failure,__ATOMIC_ACQUIRE));
    }
    configASSERT(xTaskGetTickCount()>=32);
    platform_exit(0);
}
void vApplicationStackOverflowHook(TaskHandle_t task,char *name)
{(void)task;platform_panic(name,2);}
void a733_smp_diagnostic_start(void)
{
    for(unsigned c=0;c<8;++c) {
        configASSERT(xTaskCreateStaticAffinitySet(worker,"fp",2048,(void *)(uintptr_t)c,3,
            stacks[c],&tasks[c],1UL<<c));
        configASSERT(xTaskCreateStaticAffinitySet(background,"busy",2048,(void *)(uintptr_t)c,1,
            stacks[8+c],&tasks[8+c],1UL<<c));
    }
#ifdef SUNSTONE_A733_PERF_SESSION
    /* The board-session measurements run before the workload starts. */
    a733_perf_session_start(tasks,&stacks[0][0],2048);
#endif
    vTaskStartScheduler();platform_panic("scheduler returned",3);
}
