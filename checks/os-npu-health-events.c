/* Actual OS thermal/failure event producers; scheduler effects are modeled. */
#include "../apps/os/npu.c"
#include "../apps/os/npu-declaration.c"
#include "../runtime/npu_app.c"
#include "npu-queue-unlinked.c"
#include <assert.h>
#include <stdio.h>
struct board_application board;
struct board_provider providers;
static unsigned client_events,bootstrap_events,failures;
BaseType_t xTaskGenericNotify(TaskHandle_t task,UBaseType_t index,uint32_t value,eNotifyAction action,uint32_t *previous)
{
    assert(!index&&!value&&action==eIncrement&&!previous);
    if(task==npu.client)++client_events;
    else {assert(task==npu.bootstrap);++bootstrap_events;}
    return pdPASS;
}
void os_application_wake_main(void){}   /* the application's end wait (apps/os/application.c); no application runs here */
int os_task_at_el1(void){return 1;}   /* the failure records are made as the NPU's own (EL1) tasks here */
void board_fail_note(uint32_t site,uint32_t value,const void *where)
{assert(site);(void)value;(void)where;}
void board_fail(void *unused){assert(!unused);++failures;}
/* A supervised session: the lifetime the NPU service reads (apps/os/lifetime.c, apps/os/sealed.c); it reports no
 * resident progress. */
unsigned board_session_lifetime(void){return A733_LIFETIME_SUPERVISED;}
unsigned os_lifetime(void){return A733_LIFETIME_SUPERVISED;}
void board_service_progress(unsigned service){(void)service;}
void board_fail_at(uint32_t site,uint32_t value,const void *where)
{board_fail_note(site,value,where);board_fail(NULL);}
static int run(unsigned model,const void *in,size_t ni,void *out,size_t no,uint64_t timeout)
{(void)model;(void)in;(void)ni;(void)out;(void)no;(void)timeout;return 1;}
static int close_backend(void){return 1;}
static struct npu_runtime_slot slot_zero; /* the runtime's slot table is carved at prepare; no runtime is prepared here */
static struct os_npu_device device; /* what a granted admission takes from the ledger */
int main(void)
{
    npu.device=&device;device.runtime.service.signals=&npu.signals;npu.device->runtime.slot=&slot_zero;
    npu.started=1;npu.end=10000000;
    npu.client=(TaskHandle_t)&npu.client;npu.bootstrap=(TaskHandle_t)&npu.bootstrap;
    atomic_store(&npu.gate,(UINT64_C(1000000)<<3)|ALLOW);
    os_npu_thermal(1,0,1025000,25000);
    assert(client_events==1&&!bootstrap_events);
    assert(atomic_load(&npu.gate)==(UINT64_C(1025000)<<3|ALLOW));
    os_npu_thermal(0,0,1050000,50000);
    assert(client_events==2&&bootstrap_events==1);
    os_npu_thermal(0,1,0,75000);
    assert(client_events==3&&bootstrap_events==2&&(atomic_load(&npu.gate)&STOP));
    const uint64_t stop=atomic_load(&npu.gate);
    os_npu_thermal(1,0,1100000,100000);
    assert(client_events==3&&bootstrap_events==2&&atomic_load(&npu.gate)==stop);
    assert(ss_npu_service_bind_event(sdk_notify,NULL));
    const struct ss_npu_model model={"copy",1,1,1,1};
    const struct ss_npu_backend backend={run,close_backend,&model,1,NULL};assert(ss_npu_service_bind(&backend));
    char input=1,output=0;
    assert(ss_npu_run(0,&input,1,&output,1,1)==0&&client_events==4);
    assert(ss_npu_run(0,&input,1,&output,1,1)==0&&client_events==4);
    assert(ss_npu_service_step()==1&&ss_npu_run(0,&input,1,&output,1,1)==1);
    assert(fail()==-1&&failures==1&&client_events==5&&atomic_load(&npu.failed));
    puts("PASS actual OS freshness, policy, terminal STOP, mailbox publication and failure wakeups");
}
