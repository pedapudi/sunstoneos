/* Run the actual application producer and GEMM on eight host threads. Only
 * scheduler, hardware busy/IRQ effects and the SDK transport are fixtures. */
#define _POSIX_C_SOURCE 200809L
#include "FreeRTOS.h"
#include "task.h"
#include "sunstone_app.h"
#include "npu.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static _Thread_local unsigned core=6;
static uint64_t began;
static const char *scenario,*destination;
static struct ss_grant grant={.main_core=6,.worker_cores=0xbf};
static uint64_t ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;}
uint64_t ss_counter_ticks(void){return UINT64_C(24000000)+(ns()-began)*24/1000*4;}
unsigned host_core(void){return !strcmp(scenario,"wrong-core")&&core==3?2:core;}
const struct ss_grant *ss_grant(void){return &grant;}
int ss_live(void){return 1;}
/* Model the OS: throttled until the clock grant completes 30 ms after start,
 * as the initial THROTTLE state holds until every sensor has been read. */
static int ready_now(void){return (ns()-began)>30000000;}
int ss_throttled(void){return !ready_now()||(!strcmp(scenario,"throttle-after-ready")&&(ns()-began)>120000000);}
void ss_progress(void){}
int ss_clock_ready(void){return ready_now();}
int ss_clock_info(char *line,size_t n){return snprintf(line,n,"CLOCK cpu_l_mhz=1794 cpu_b_mhz=2002 cpu_l_mv=1000 cpu_b_mv=1050 transitions=3 dsu_mhz=1274 sid=00000000 bin=4 core=6 pattern=0 parked_max_us=1\n");}
void ss_activity_begin(void){}
void ss_activity_end(int success){if(!success)abort();}
_Noreturn void ss_fail_value(uint32_t detail,uint32_t value){fprintf(stderr,"FIRST_FAULT detail=%08x value=%08x\n",detail,value);fflush(stderr);_Exit(2);}
_Noreturn void ss_fail(uint32_t detail){ss_fail_value(detail,detail);}
_Noreturn void ss_finish(const void *data,size_t bytes){FILE *f=fopen(destination,"wb");if(!f||fwrite(data,1,bytes,f)!=bytes||fclose(f))abort();_Exit(0);}
static void *entry(void *argument){StaticTask_t *task=argument;core=task->core;if(!strcmp(scenario,"frozen-cpu1")&&core==1){struct timespec pause={0,200000000};nanosleep(&pause,NULL);}task->entry(task->argument);return NULL;}
TaskHandle_t xTaskCreateStaticAffinitySet(void (*fn)(void *),const char *name,unsigned words,void *arg,unsigned priority,StackType_t *stack,StaticTask_t *task,unsigned mask)
{(void)name;(void)words;(void)priority;(void)stack;task->core=__builtin_ctz(mask);task->entry=fn;task->argument=arg;if(pthread_create(&task->thread,NULL,entry,task))return NULL;return task;}
TaskHandle_t xTaskGetCurrentTaskHandle(void){return NULL;}
int xTaskGetSchedulerState(void){return 0;}
void vTaskDelay(unsigned ticks){struct timespec t={0,(long)(ticks?ticks:1)*10000};nanosleep(&t,NULL);}
void vTaskSuspend(void *unused){(void)unused;pthread_exit(NULL);}
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition=PTHREAD_COND_INITIALIZER;
static atomic_uint request,answer,completions,busy;
static unsigned jobs,kicks;
static uint64_t irq_tick;
static struct npu_runtime runtime;
static StaticTask_t service_task;
static void (*observer)(void *,const struct npu_service *,unsigned);
static void *observer_cookie;
static uint8_t input_data[32],reference_data[2]={0,0xff};
const uint8_t cpu_npu_input[32]={0},cpu_npu_reference[2]={0,0xff};
const size_t cpu_npu_input_bytes=32,cpu_npu_reference_bytes=2;
const struct os_npu_vector os_npu_inputs[]={{input_data,sizeof(input_data)}};
const struct os_npu_vector os_npu_references[]={{reference_data,sizeof(reference_data)}};
const unsigned os_npu_input_count=1,os_npu_output_count=1;
void xTaskNotifyGive(TaskHandle_t task){(void)task;pthread_mutex_lock(&lock);++kicks;pthread_cond_signal(&condition);pthread_mutex_unlock(&lock);}
int ss_npu_model_find(const char *name,unsigned *index){if(strcmp(name,"shufflenet-v2-published"))return 0;*index=0;return 1;}
int ss_npu_model_info(unsigned index,struct ss_npu_model *m){if(index)return 0;memset(m,0,sizeof(*m));m->inputs=1;m->outputs=1;m->input_bytes=sizeof(input_data);m->output_bytes=2;return 1;}
int npu_service_observe(struct npu_service *s,void (*fn)(void *,const struct npu_service *,unsigned),void *cookie){(void)s;observer=fn;observer_cookie=cookie;return 1;}
unsigned npu_service_irq_completions(const struct npu_service *s){(void)s;return atomic_load(&completions);}
uint64_t npu_application_irq_ticks(void){return irq_tick;}
unsigned npu_application_irq_count(void){return atomic_load(&completions);}
static int idle(void *cookie,unsigned offset,uint32_t *value){(void)cookie;if(offset!=4)abort();*value=atomic_load(&busy)?0:0x7fffffff;return 1;}
static void *service(void *unused)
{
    (void)unused;core=1;
    for(;;){
        pthread_mutex_lock(&lock);
        while(atomic_load(&request)!=1)pthread_cond_wait(&condition,&lock);
        pthread_mutex_unlock(&lock);
        runtime.service.controller.state=3;runtime.service.controller.job.token=(void *)(uintptr_t)0x12340000;
        runtime.service.trigger_ticks=ss_counter_ticks();runtime.service.member[0].wait.ticket=++jobs;
        atomic_store(&busy,1);observer(observer_cookie,&runtime.service,1);
        if(!strcmp(scenario,"early-complete"))atomic_store(&busy,0);
        uint64_t until=ns()+10000000;
        for(;;){
            pthread_mutex_lock(&lock);
            if(ns()>=until){pthread_mutex_unlock(&lock);break;}
            struct timespec deadline;clock_gettime(CLOCK_REALTIME,&deadline);
            deadline.tv_nsec+=1000000;if(deadline.tv_nsec>=1000000000){++deadline.tv_sec;deadline.tv_nsec-=1000000000;}
            pthread_cond_timedwait(&condition,&lock,&deadline);
            pthread_mutex_unlock(&lock);
            if(!strcmp(scenario,"changed-ticket"))++runtime.service.member[0].wait.ticket;
            if(!strcmp(scenario,"changed-token"))runtime.service.controller.job.token=(void *)(uintptr_t)0x56780000;
            observer(observer_cookie,&runtime.service,0);
        }
        atomic_store(&busy,0);irq_tick=ss_counter_ticks();atomic_fetch_add(&completions,1);
        observer(observer_cookie,&runtime.service,2);
        atomic_store(&request,2);
    }
}
int ss_npu_run(unsigned model,const void *input,size_t input_bytes,void *output,size_t output_bytes,uint64_t timeout)
{
    if(model||input_bytes!=32||output_bytes!=2||timeout!=100000||memcmp(input,input_data,32))abort();
    if(atomic_load(&request)==2){memcpy(output,reference_data,2);atomic_store(&request,0);return 1;}
    unsigned clear=0;if(atomic_compare_exchange_strong(&request,&clear,1))xTaskNotifyGive(&service_task);
    return 0;
}
int ss_npu_close(void){if(atomic_load(&request))abort();return 1;}
extern void cpu_npu_load_init(void),cpu_npu_load_main(const struct ss_grant *);
extern int os_npu_diagnostic_bind(struct npu_runtime *);
int main(int argc,char **argv)
{
    if(argc!=3)return 4;scenario=argv[1];destination=argv[2];began=ns();
    grant.finish_by_us=ss_counter_ticks()/24+8000000;
    cpu_npu_load_init();
#ifdef SUNSTONE_A733_OS_NPU
    runtime.service.ops.read=idle;runtime.service.worker=&service_task;
    if(!os_npu_diagnostic_bind(&runtime))abort();
    if(pthread_create(&service_task.thread,NULL,service,NULL))abort();
#endif
    cpu_npu_load_main(&grant);return 3;
}
