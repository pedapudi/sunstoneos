/* Run the actual application producer and GEMM on eight host threads. Only
 * scheduler, hardware busy/IRQ effects and the SDK transport are fixtures.
 *
 * Time is virtual, so no result depends on how the host schedules the threads. The eight tasks (main on core 6, the workers
 * on the other cores) and the NPU service thread each keep a virtual clock and run one at a time: at every yield point the
 * thread with the smallest clock runs next, ties to the lower slot, the service last. No thread therefore reads memory that
 * another wrote at a later virtual time, and the threads behave as eight cores running side by side. The yield points are
 * the fixture functions the application and the observer call: ss_counter_ticks advances the caller by COUNTER_COST,
 * vTaskDelay by one board tick (1 ms, configTICK_RATE_HZ 1000) per tick, and the service's 1 ms poll by 1 ms. A notification
 * wakes a waiting thread at the notifier's time. Every wait loop of the application calls one of them. A run where no thread
 * can run stops with VIRTUAL_DEADLOCK instead of hanging. The 5 s work cutoff, the drain after it and the 30 ms clock-ready
 * delay are therefore the same on every run, and two runs write the same bytes. */
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
static const char *scenario,*destination;
static struct ss_span spans[2];
static struct ss_grant grant={.main_core=6,.worker_cores=0xbf,.regions=spans};
/* The virtual scheduler. Slots 0-7 are the tasks of cores 0-7 and slot 8 the NPU service; a slot's clock is the virtual time
 * at which it next runs (WAITING: until a notification). */
#define SLOTS 9u
#define SERVICE_SLOT 8u
#define COUNTER_BASE UINT64_C(24000000)     /* the counter at the start of the run */
#define COUNTER_COST UINT64_C(12000)        /* ticks one counter read costs its caller: 0.5 ms, three per worker iteration */
#define TICK UINT64_C(24000)                /* one board tick of vTaskDelay: 1 ms */
#define WAITING UINT64_MAX
#define VIRTUAL_DEADLOCK 5
enum { ABSENT, LIVE, EXITED };
static struct { pthread_cond_t turn; uint64_t clock; unsigned state; } slot[SLOTS];
static pthread_mutex_t big=PTHREAD_MUTEX_INITIALIZER;
static unsigned running=6;
static _Thread_local unsigned self=6;
static atomic_uint thaw;
/* The frozen core of the frozen-cpu1 scenario makes no progress until the first busy window has closed. */
static int may_run(unsigned k)
{return slot[k].state==LIVE&&slot[k].clock!=WAITING&&!(k==1&&!strcmp(scenario,"frozen-cpu1")&&!atomic_load(&thaw));}
/* With big held: hand the turn to the slot that runs next, and return when it is this thread's turn again. */
static void pass_turn(void)
{
    unsigned next=SLOTS;
    for(unsigned k=0;k<SLOTS;++k)if(may_run(k)&&(next==SLOTS||slot[k].clock<slot[next].clock))next=k;
    if(next==SLOTS){fprintf(stderr,"VIRTUAL_DEADLOCK no thread can run\n");fflush(stderr);_Exit(VIRTUAL_DEADLOCK);}
    running=next;
    if(next!=self){
        pthread_cond_signal(&slot[next].turn);
        while(running!=self)pthread_cond_wait(&slot[self].turn,&big);
    }
}
/* Advance this thread's clock and yield; return the clock when it runs again. */
static uint64_t advance(uint64_t ticks)
{
    pthread_mutex_lock(&big);
    slot[self].clock+=ticks;pass_turn();const uint64_t now=slot[self].clock;
    pthread_mutex_unlock(&big);
    return now;
}
/* Wait for a notification, or until `ticks` have passed when ticks is nonzero; the clock when this thread runs again. */
static uint64_t wait_notified(uint64_t ticks)
{
    pthread_mutex_lock(&big);
    slot[self].clock=ticks?slot[self].clock+ticks:WAITING;pass_turn();const uint64_t now=slot[self].clock;
    pthread_mutex_unlock(&big);
    return now;
}
/* With big held: a waiting slot runs no later than the notifier's time. */
static void wake(unsigned k){if(slot[self].clock<slot[k].clock)slot[k].clock=slot[self].clock;}
static uint64_t now_ticks(void){return slot[self].clock;}   /* the caller's time, without yielding */
uint64_t ss_counter_ticks(void){return COUNTER_BASE+advance(COUNTER_COST);}
unsigned host_core(void){return !strcmp(scenario,"wrong-core")&&core==3?2:core;}
const struct ss_grant *ss_grant(void){return &grant;}
int ss_live(void){return 1;}
/* Model the OS: throttled until the clock grant completes 30 ms after start,
 * as the initial THROTTLE state holds until every sensor has been read. */
static int ready_now(void){return now_ticks()>UINT64_C(720000);}
static atomic_uint started;
int ss_throttled(void){return !ready_now()||(!strcmp(scenario,"throttle-after-ready")&&now_ticks()>UINT64_C(2880000))||(!strcmp(scenario,"throttle")&&atomic_load(&started));}

void ss_thermal_status(struct ss_thermal_status *s)
{
    memset(s,0,sizeof(*s));s->seen=31;s->sequence=7;s->start_us=1000;s->now_us=5000000;
    s->millicelsius[0]=41000;s->millicelsius[1]=42000;s->millicelsius[2]=-3000;s->millicelsius[3]=44000;s->millicelsius[4]=45000;
    if(!strcmp(scenario,"throttle")){s->cause=2;s->deadline_us=4999000;}else s->deadline_us=5001000;
}
void ss_progress(void){}
int ss_clock_ready(void){return ready_now();}
int ss_clock_info(char *line,size_t n){return snprintf(line,n,"CLOCK cpu_l_mhz=1794 cpu_b_mhz=2002 cpu_l_mv=1000 cpu_b_mv=1050 transitions=3 dsu_mhz=1274 sid=00000000 bin=4 core=6 pattern=0 parked_max_us=1\n");}
void ss_activity_begin(void){atomic_store(&started,1);}
void ss_activity_end(int success){if(!success)abort();}
_Noreturn void ss_fail_value(uint32_t detail,uint32_t value){fprintf(stderr,"FIRST_FAULT detail=%08x value=%08x\n",detail,value);fflush(stderr);_Exit(2);}
_Noreturn void ss_fail(uint32_t detail){ss_fail_value(detail,detail);}
static atomic_uint sdk_closed;
/* The OS waits in ss_finish for the NPU client's report, which only ss_npu_close publishes: a session that grants the NPU and
 * finishes without closing it stops there (apps/os/application.c, detail 0x75f) instead of returning its record. */
_Noreturn void ss_finish(const void *data,size_t bytes){
#ifdef CPU_NPU_WITH_NPU
    if(!atomic_load(&sdk_closed))ss_fail_value(0x75f,0);
#endif
    FILE *f=fopen(destination,"wb");if(!f||fwrite(data,1,bytes,f)!=bytes||fclose(f))abort();_Exit(0);}
/* A thread enters the scheduler and waits for its first turn. */
static void enter(unsigned k)
{
    self=k;pthread_mutex_lock(&big);
    while(running!=self)pthread_cond_wait(&slot[self].turn,&big);
    pthread_mutex_unlock(&big);
}
/* With big held: a new slot starts at its creator's time. */
static void start_slot(unsigned k)
{
    if(k>=SLOTS||slot[k].state!=ABSENT)abort();
    pthread_cond_init(&slot[k].turn,NULL);slot[k].clock=slot[self].clock;slot[k].state=LIVE;
}
static void *entry(void *argument){StaticTask_t *task=argument;core=task->core;enter(task->core);task->entry(task->argument);return NULL;}
TaskHandle_t xTaskCreateStaticAffinitySet(void (*fn)(void *),const char *name,unsigned words,void *arg,unsigned priority,StackType_t *stack,StaticTask_t *task,unsigned mask)
{
    (void)name;(void)words;(void)priority;(void)stack;task->core=__builtin_ctz(mask);task->entry=fn;task->argument=arg;
    pthread_mutex_lock(&big);start_slot(task->core);pthread_mutex_unlock(&big);
    if(pthread_create(&task->thread,NULL,entry,task))return NULL;
    return task;
}
/* The kit's task creation on the host: the same thread, no address space. */
void *ss_task_create_static_affinity_set(void (*fn)(void *),const char *name,uint32_t words,void *arg,unsigned long priority,void *stack,void *task,unsigned long mask)
{return xTaskCreateStaticAffinitySet(fn,name,words,arg,(unsigned)priority,stack,task,(unsigned)mask);}
TaskHandle_t xTaskGetCurrentTaskHandle(void){return NULL;}
int xTaskGetSchedulerState(void){return 0;}
void vTaskDelay(unsigned ticks){(void)advance((uint64_t)(ticks?ticks:1)*TICK);}
/* A suspended task leaves the scheduler for good. */
void vTaskSuspend(void *unused)
{
    (void)unused;pthread_mutex_lock(&big);slot[self].state=EXITED;
    unsigned next=SLOTS;
    for(unsigned k=0;k<SLOTS;++k)if(may_run(k)&&(next==SLOTS||slot[k].clock<slot[next].clock))next=k;
    if(next==SLOTS){fprintf(stderr,"VIRTUAL_DEADLOCK no thread can run\n");fflush(stderr);_Exit(VIRTUAL_DEADLOCK);}
    running=next;pthread_cond_signal(&slot[next].turn);pthread_mutex_unlock(&big);pthread_exit(NULL);
}
static atomic_uint request,completions,busy,idle_reads;
static unsigned jobs,kicks;
static uint64_t irq_tick;
static struct npu_runtime runtime;
static StaticTask_t service_task;
static void (*observer)(void *,const struct npu_service *,unsigned);
static void *observer_cookie;
static uint8_t input_data[32],reference_data[2]={0,0xff};
const uint8_t cpu_npu_reference[2]={0,0xff};
const size_t cpu_npu_reference_bytes=2;
/* The runner's two input regions: the models region (an index of one graph of four bytes, as tools/npu-external-pack.py
 * writes it, and the graph on the next page) and the vectors region (the input tensor). */
static uint8_t models_region[8192]={'N','M','D','L',1,0,0,0, 1,0,0,0,0,0,0,0, 0,0x10,0,0,0,0,0,0, 4,0,0,0,0,0,0,0};
static uint8_t vectors_region[32];
const struct os_npu_vector os_npu_inputs[]={{input_data,sizeof(input_data)}};
const struct os_npu_vector os_npu_references[]={{reference_data,sizeof(reference_data)}};
const unsigned os_npu_input_count=1,os_npu_output_count=1;
/* The only task notified is the NPU service (the SDK's submission and the observer's kick). */
void xTaskNotifyGive(TaskHandle_t task){(void)task;pthread_mutex_lock(&big);++kicks;wake(SERVICE_SLOT);pthread_mutex_unlock(&big);}
int ss_npu_register_graph(const void *graph,size_t bytes,unsigned *index){if(!graph||!bytes)return -1;*index=0;return 1;}
/* As the OS mailbox does: the description of a registered graph is pending until the NPU task answers it. */
static atomic_uint info_asked;
int ss_npu_model_info(unsigned index,struct ss_npu_model *m){if(index||!m)return -1;if(!atomic_fetch_add(&info_asked,1))return 0;memset(m,0,sizeof(*m));m->inputs=1;m->outputs=1;m->input_bytes=sizeof(input_data);m->output_bytes=2;return 1;}
int npu_service_observe(struct npu_service *s,void (*fn)(void *,const struct npu_service *,unsigned),void *cookie){(void)s;observer=fn;observer_cookie=cookie;return 1;}
unsigned npu_service_irq_completions(const struct npu_service *s){(void)s;return atomic_load(&completions);}
uint64_t npu_application_irq_ticks(void){return irq_tick;}
unsigned npu_application_irq_count(void){return atomic_load(&completions);}
static int idle(void *cookie,unsigned offset,uint32_t *value){(void)cookie;if(offset!=4)abort();atomic_fetch_add(&idle_reads,1);*value=atomic_load(&busy)?0:0x7fffffff;return 1;}
static void *service(void *unused)
{
    (void)unused;core=1;enter(SERVICE_SLOT);
    for(;;){
        while(atomic_load(&request)!=1)(void)wait_notified(0);
        runtime.service.controller.state=3;runtime.service.controller.job.token=(void *)(uintptr_t)0x12340000;
        runtime.service.trigger_ticks=ss_counter_ticks();runtime.service.member[0].wait.ticket=++jobs;
        atomic_store(&idle_reads,0);atomic_store(&busy,1);observer(observer_cookie,&runtime.service,1);
        if(!strcmp(scenario,"early-complete"))atomic_store(&busy,0);
        /* The busy window is defined by events, not host time. The application reads the idle register
         * again only after it accepted a candidate sample, that is after every worker core finished a chunk
         * while the job was busy. A passing run holds the job busy until that second read, however long a
         * loaded host takes to run the workers. A scenario that removes the overlap (early completion, a changed
         * ticket or token, a frozen core) has none to wait for, so its window is ten polls. A hang guard of ten seconds of the
         * service's virtual time only bounds a defective run. Each poll waits 1 ms or until a notification. */
        const int expect_overlap=strcmp(scenario,"early-complete")&&strcmp(scenario,"changed-ticket")&&
            strcmp(scenario,"changed-token")&&strcmp(scenario,"frozen-cpu1");
        const uint64_t guard=now_ticks()+UINT64_C(240000000);unsigned polls=0;
        for(;;){
            if((expect_overlap?atomic_load(&idle_reads)>=2:polls>=10)||now_ticks()>=guard)break;
            ++polls;
            (void)wait_notified(TICK);
            if(!strcmp(scenario,"changed-ticket"))++runtime.service.member[0].wait.ticket;
            if(!strcmp(scenario,"changed-token"))runtime.service.controller.job.token=(void *)(uintptr_t)0x56780000;
            observer(observer_cookie,&runtime.service,0);
        }
        if(!atomic_exchange(&thaw,1)){pthread_mutex_lock(&big);if(slot[1].clock<slot[self].clock)slot[1].clock=slot[self].clock;pthread_mutex_unlock(&big);}
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
int ss_npu_close(void){if(atomic_load(&request)==1)return 0;atomic_store(&sdk_closed,1);return 1;}
extern void cpu_npu_load_init(void),cpu_npu_load_main(const struct ss_grant *);
extern int os_npu_diagnostic_bind(struct npu_runtime *);
/* The session of this model declares observation (OS_NPU_OBSERVE). */
int os_npu_observed(void){return 1;}
int main(int argc,char **argv)
{
    if(argc!=3)return 4;scenario=argv[1];destination=argv[2];
    pthread_cond_init(&slot[6].turn,NULL);slot[6].state=LIVE;   /* this thread is the main task on core 6, at time 0 */
    grant.finish_by_us=ss_counter_ticks()/24+8000000;
    grant.region_count=2;
    spans[0]=(struct ss_span){models_region,sizeof(models_region)};
    spans[1]=(struct ss_span){vectors_region,sizeof(vectors_region)};
    cpu_npu_load_init();
#ifdef CPU_NPU_WITH_NPU
    runtime.service.ops.read=idle;runtime.service.worker=&service_task;
    if(!os_npu_diagnostic_bind(&runtime))abort();
    pthread_mutex_lock(&big);start_slot(SERVICE_SLOT);slot[SERVICE_SLOT].clock=WAITING;pthread_mutex_unlock(&big);
    if(pthread_create(&service_task.thread,NULL,service,NULL))abort();
#endif
    cpu_npu_load_main(&grant);return 3;
}
