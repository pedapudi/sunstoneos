/* SPDX-License-Identifier: GPL-2.0-only */
#include "sunstone_app.h"
#include "FreeRTOS.h"
#include "task.h"
#include "result.h"
_Static_assert(configNUMBER_OF_CORES==CAPACITY_CORES,"eight physical cores required");
_Static_assert(sizeof(struct capacity_result)<=65536,"output grant");
/* Public wrapper supplied by the OS; tasks only read the boot snapshot. */
extern int ss_cpu_observations(unsigned,struct ss_cpu_observation *);
static struct capacity_result result;
static _Alignas(64) uint32_t data[CAPACITY_WORDS];
static StaticTask_t state[CAPACITY_CORES];
static StackType_t stack[CAPACITY_CORES][1024];
static TaskHandle_t workers[CAPACITY_CORES],controller;
static uint64_t counter(void)
{uint64_t v;__asm__ volatile("isb; mrs %0,cntpct_el0":"=r"(v)::"memory");return v;}
static void worker(void *arg)
{
    unsigned core=(unsigned)(uintptr_t)arg;
    for(;;){
        (void)ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
        struct capacity_core *c=&result.core[core];c->observed_core=portGET_CORE_ID();
        if(c->observed_core!=core)ss_fail(0xca10|core);
        for(unsigned kernel=0;kernel<2;++kernel){
            uint64_t (*run)(uint32_t *)=kernel?capacity_neon:capacity_integer;
            for(unsigned rep=0;rep<CAPACITY_WARM+CAPACITY_REPS;++rep){
                uint64_t start=counter(),sum=run(data),end=counter();
                if(rep>=CAPACITY_WARM)c->samples[kernel][rep-CAPACITY_WARM]=
                    (struct capacity_sample){end-start,sum};
            }
        }
        xTaskNotifyGive(controller);
    }
}
void cpu_capacity_init(void)
{
    const struct ss_grant *g=ss_grant();
    if(!g||g->main_core!=6||g->worker_cores!=0xbf)ss_fail(0xca01);
    for(unsigned core=0;core<CAPACITY_CORES;++core){
        workers[core]=xTaskCreateStaticAffinitySet(worker,"capacity",1024,
            (void *)(uintptr_t)core,2,stack[core],&state[core],1u<<core);
        if(!workers[core])ss_fail(0xca02);
    }
}
void cpu_capacity_main(const struct ss_grant *g)
{
    (void)g;controller=xTaskGetCurrentTaskHandle();
    while(!ss_clock_ready()||ss_throttled()){
        if(ss_live()<0)ss_fail(0xca03);ss_progress();vTaskDelay(1);
    }
    result.magic=CAPACITY_MAGIC;
    __asm__ volatile("mrs %0,cntfrq_el0":"=r"(result.counter_hz));
    result.words=CAPACITY_WORDS;result.rounds=CAPACITY_ROUNDS;
    result.warm=CAPACITY_WARM;result.reps=CAPACITY_REPS;
    ss_activity_begin();
    for(unsigned core=0;core<CAPACITY_CORES;++core){
        struct capacity_core *c=&result.core[core];
        if(!ss_cpu_observations(core,&c->observation))ss_fail(0xca20|core);
        if(ss_clock_info(c->clock_before,sizeof(c->clock_before))<=0)ss_fail(0xca30|core);
        c->readiness=(ss_clock_ready()?1u:0u)|(!ss_throttled()?2u:0u);
        xTaskNotifyGive(workers[core]);
        if(ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(1000))!=1)ss_fail(0xca40|core);
        if(ss_clock_info(c->clock_after,sizeof(c->clock_after))<=0)ss_fail(0xca50|core);
        c->readiness|=(ss_clock_ready()?4u:0u)|(!ss_throttled()?8u:0u);
        if(ss_live()<0)ss_fail(0xca60|core);
        ss_progress();
    }
    ss_activity_end(1);ss_finish(&result,sizeof(result));
}
