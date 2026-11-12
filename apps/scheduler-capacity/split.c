/* SPDX-License-Identifier: GPL-2.0-only */
#include "sunstone_app.h"
#include "FreeRTOS.h"
#include "task.h"
#include "split-result.h"
#include <stdatomic.h>
#include <string.h>
_Static_assert(configNUMBER_OF_CORES==CAPACITY_CORES,"eight-core diagnostic");
_Static_assert(configTASK_NOTIFICATION_ARRAY_ENTRIES>=2,"private completion notification");
static _Alignas(64) struct split_result result;
static _Alignas(64) struct split_phase warmup;
static _Alignas(64) uint32_t data[CAPACITY_CORES][CAPACITY_WORDS];
static StaticTask_t states[CAPACITY_CORES];
static StackType_t stacks[CAPACITY_CORES][1024];
static TaskHandle_t workers[CAPACITY_CORES],controller;
static _Atomic(struct split_phase *) current;
static struct ss_work_range ranges[2][CAPACITY_CORES];
static uint64_t counter(void)
{uint64_t v;__asm__ volatile("isb; mrs %0,cntpct_el0":"=r"(v)::"memory");return v;}
static int top_clock(char *line)
{
    static const char prefix[]="CLOCK cpu_l_mhz=1794 cpu_b_mhz=2002 cpu_l_mv=1000 cpu_b_mv=1050 transitions=";
    return ss_clock_ready()&&!ss_throttled()&&ss_clock_info(line,256)>0&&
        !strncmp(line,prefix,sizeof(prefix)-1)&&strstr(line," dsu_mhz=1274 ");
}
static void worker(void *arg)
{
    unsigned c=(unsigned)(uintptr_t)arg;
    for(;;){
        (void)ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
        struct split_phase *p=atomic_load_explicit(&current,memory_order_acquire);
        struct split_core *s=&p->core[c];
        s->observed_core=portGET_CORE_ID();
        if(s->observed_core!=c)ss_fail(0xcb10|c);
        s->started=counter();uint64_t digest=0;
        for(uint64_t tile=s->begin;tile<s->end;++tile)
            digest+=capacity_integer(data[c])^tile;
        s->digest=digest;s->finished=counter();
        xTaskNotifyGiveIndexed(controller,1);
    }
}
/* Choose the immutable maximum weights only for the verified top-point window. */
static void prepare_ranges(const struct ss_grant *g)
{
    struct ss_cpu_topology_info info;
    struct ss_cpu_info cpus[CAPACITY_CORES];
    size_t count=0;
    if(ss_cpu_topology_info(&info)||info.cpu_count!=CAPACITY_CORES||
       !info.calibration_epoch||
       ss_cpu_topology(cpus,CAPACITY_CORES,&count)||count!=CAPACITY_CORES)ss_fail(0xcb20);
    uint64_t mask=g->worker_cores|(UINT64_C(1)<<g->main_core);
    for(unsigned c=0;c<CAPACITY_CORES;++c){
        if(!(cpus[c].flags&SS_CPU_CAPACITY_MAX_VALID)||!cpus[c].capacity_max||
           cpus[c].performance_domain>=info.domain_count)ss_fail(0xcb21|c);
        result.capacity[c]=cpus[c].capacity_max;
        ranges[0][c]=(struct ss_work_range){SPLIT_UNITS*c/CAPACITY_CORES,
            SPLIT_UNITS*(c+1)/CAPACITY_CORES,c,0};
    }
    if(ss_work_split_max(SPLIT_UNITS,mask,1,ranges[1],CAPACITY_CORES,&count)||
       count!=CAPACITY_CORES)ss_fail(0xcb30);
}
static void run_phase(struct split_phase *p,unsigned mode)
{
    memset(p,0,sizeof(*p));
    for(unsigned c=0;c<CAPACITY_CORES;++c){
        if(ranges[mode][c].cpu!=c)ss_fail(0xcb31);
        p->core[c].begin=ranges[mode][c].begin;p->core[c].end=ranges[mode][c].end;
    }
    p->readiness=ss_clock_ready()&&!ss_throttled()?1:0;
    atomic_store_explicit(&current,p,memory_order_release);p->launch=counter();
    for(unsigned c=0;c<CAPACITY_CORES;++c)xTaskNotifyGive(workers[c]);
    unsigned done=0;
    while(done<CAPACITY_CORES){
        unsigned n=ulTaskNotifyTakeIndexed(1,pdTRUE,portMAX_DELAY);
        if(n>CAPACITY_CORES-done)ss_fail(0xcb40);done+=n;
    }
    if(ss_clock_ready()&&!ss_throttled())p->readiness|=2;
    if(ss_live()<0)ss_fail(0xcb41);ss_progress();
}
void cpu_split_init(void)
{
    const struct ss_grant *g=ss_grant();
    if(!g||g->main_core!=6||g->worker_cores!=0xbf)ss_fail(0xcb01);
    for(unsigned c=0;c<CAPACITY_CORES;++c){
        workers[c]=ss_task_create_static_affinity_set(worker,"capacity-split",1024,
            (void *)(uintptr_t)c,2,stacks[c],&states[c],1u<<c);
        if(!workers[c])ss_fail(0xcb02);
    }
}
void cpu_split_main(const struct ss_grant *g)
{
    controller=xTaskGetCurrentTaskHandle();
    /* Initial service startup is outside every measured job window. */
    while(!ss_clock_ready()||ss_throttled()){
        if(ss_live()<0)ss_fail(0xcb03);ss_progress();vTaskDelay(1);
    }
    if(!top_clock(result.clock_before))ss_fail(0xcb04);
    prepare_ranges(g);
    result.magic=SPLIT_MAGIC;
    __asm__ volatile("mrs %0,cntfrq_el0":"=r"(result.counter_hz));
    result.units=SPLIT_UNITS;result.warm=SPLIT_WARM;result.reps=SPLIT_REPS;result.cores=CAPACITY_CORES;
    ss_activity_begin();
    for(unsigned rep=0;rep<SPLIT_WARM+SPLIT_REPS;++rep)
        for(unsigned mode=0;mode<2;++mode)
            run_phase(rep<SPLIT_WARM?&warmup:&result.samples[mode][rep-SPLIT_WARM],mode);
    if(!top_clock(result.clock_after)||strcmp(result.clock_before,result.clock_after))ss_fail(0xcb05);
    ss_activity_end(1);ss_finish(&result,sizeof(result));
}
