/* Optional diagnostic sampling on the serialized runtime worker. The app
 * callback receives ordinary data; this adapter owns all register access. */
#include "npu.h"
#include "sunstone_app.h"
#include "a733_npu_irq.h"
#ifdef SUNSTONE_A733_OS_NPU_OBSERVE
static struct npu_runtime *runtime;
static int (*callback)(void *,unsigned,const struct ss_npu_observation *);
static void *context;
static struct ss_npu_observation running;
static unsigned first_irq;
int ss_npu_observe(int (*fn)(void *,unsigned,const struct ss_npu_observation *),void *cookie)
{
    if(!fn||callback||xTaskGetSchedulerState()!=taskSCHEDULER_NOT_STARTED)return 0;
    callback=fn;context=cookie;return 1;
}
void ss_npu_observer_kick(void)
{if(runtime&&runtime->service.worker)xTaskNotifyGive(runtime->service.worker);}
static int same(const struct npu_service *s)
{
    return s->controller.state==A733_NPU_RUNNING&&s->running==running.client&&
        s->member[s->running].wait.ticket==running.ticket&&
        (uintptr_t)s->controller.job.token==running.token&&s->trigger_ticks==running.trigger_ticks;
}
static int busy(const struct npu_service *s)
{
    uint32_t idle;
    if(!same(s)||!s->ops.read(s->cookie,4,&idle))return 0;
    running.sample_ticks=ss_counter_ticks();running.idle=idle;running.state=s->controller.state;
    return idle!=UINT32_C(0x7fffffff)&&same(s);
}
static void sample(void *cookie,const struct npu_service *s,unsigned event)
{
    struct npu_runtime *rt=cookie;
    if(!callback)return;
    if(event==1){
        running=(struct ss_npu_observation){
            .ticket=s->member[s->running].wait.ticket,.token=(uintptr_t)s->controller.job.token,
            .client=s->running,.model=rt->slot[s->running].model,.trigger_ticks=s->trigger_ticks};
        first_irq=npu_application_irq_count();
        if(busy(s))(void)callback(context,SS_NPU_OBSERVE_START,&running);
    }else if(event==2){
        running.irq_ticks=npu_application_irq_ticks();
        running.irq_count=npu_application_irq_count()-first_irq;
        (void)callback(context,SS_NPU_OBSERVE_DONE,&running);
    }else if(same(s)&&callback(context,SS_NPU_OBSERVE_CANDIDATE,&running)==1&&busy(s)){
        (void)callback(context,SS_NPU_OBSERVE_CONFIRMED,&running);
    }
}
int os_npu_diagnostic_bind(struct npu_runtime *rt)
{
    /* Application init precedes service starts (board_application_start).
     * An absent observer leaves the service callback null: no per-trigger
     * timestamps, callback passes or additional MMIO. */
    if(!callback)return 1;
    if(!rt||runtime)return 0;
    runtime=rt;return npu_service_observe(&rt->service,sample,rt);
}
#else
int ss_npu_observe(int (*fn)(void *,unsigned,const struct ss_npu_observation *),void *cookie)
{(void)fn;(void)cookie;return 0;}
void ss_npu_observer_kick(void){}
#endif
