#include "a733_npu_application.h"
#include "platform.h"
#include "boot.h"
#include "cached.h"
#include "result.h"
#include <string.h>
#ifdef SUNSTONE_NPU_SUPERVISED
#include "session.h"
#endif

static StaticTask_t client_state;
static StackType_t client_stack[4096] __attribute__((aligned(16)));
struct a733_npu_application_result a733_npu_application_result;

static int live(void)
{
    return !atomic_load_explicit(&a733_npu_application_failed,memory_order_acquire) &&
        a733_smp_counter()<a733_observe_identity.deadline;
}
static _Noreturn void retained(unsigned reason)
{
    a733_npu_application_fail(reason);vTaskSuspend(NULL);
    platform_panic("NPU client retained",reason);
}
static struct npu_service_reply request(struct npu_convolution *c,enum npu_service_operation operation,
    const void *input,size_t bytes,uint64_t timeout)
{
    uint64_t ticket;
    struct npu_service_reply reply;
    if(!live() || !npu_convolution_send(c,operation,input,bytes,timeout,5,&ticket))retained(6);
    for(;;) {
        if(!live())retained(7);
        if(npu_convolution_collect(c,ticket,&reply,5))break;
        /* A finite collect timeout preserves this exact outstanding ticket. */
    }
    if(atomic_load_explicit(&c->failed,memory_order_acquire) || reply.result!=NPU_SERVICE_OK || reply.fault!=A733_NPU_NO_FAULT ||
       !reply.owns_session || reply.canceled)retained(8);
    return reply;
}
static void client(void *unused)
{
    (void)unused;
    while(!a733_npu_application_ready()) {
        if(!live())retained(9);
        vTaskDelay(1);
    }
    struct npu_convolution *c=a733_npu_application_client();
    if(!c)retained(10);
    struct npu_service_reply reply=request(c,NPU_SERVICE_OPEN,NULL,0,1000000);
    while(reply.state!=A733_NPU_READY) {
        if(reply.state!=A733_NPU_BOOTING)retained(11);
        vTaskDelay(1);reply=request(c,NPU_SERVICE_STATUS,NULL,0,0);
    }
    a733_npu_application_result.start=a733_smp_counter();
    unsigned mismatch=0;uint64_t previous=0,job=0;
    for(unsigned iteration=0;iteration<2;++iteration) {
    memset(a733_npu_application_result.output,0,sizeof(a733_npu_application_result.output));
    reply=request(c,NPU_SERVICE_SUBMIT,a733_npu_application_input,a733_npu_application_input_bytes,1000000);
    job=reply.job_ticket;
    if(!job || job==previous)retained(12);
    while(reply.state!=A733_NPU_DONE) {
        if(reply.state!=A733_NPU_RUNNING || reply.job_ticket!=job)retained(13);
        vTaskDelay(1);reply=request(c,NPU_SERVICE_STATUS,NULL,0,0);
    }
    if(reply.job_ticket!=job || !npu_convolution_output(c,a733_npu_application_result.output,
        a733_npu_application_reference_bytes))retained(14);
    for(unsigned i=0;i<a733_npu_application_reference_bytes;++i)
        mismatch|=a733_npu_application_result.output[i]^a733_npu_application_reference[i];
    reply=request(c,NPU_SERVICE_REAP,NULL,0,0);
    if(reply.state!=A733_NPU_READY || !live())retained(15);
    if(iteration==0) {previous=job;
        a733_observe_digest(a733_npu_application_result.output,a733_npu_application_reference_bytes,
            a733_npu_application_result.first_digest);
    }
    }
    a733_npu_application_result.previous_job=previous;
    a733_npu_application_result.jobs=2;
    a733_npu_application_result.job=job;
    a733_npu_application_result.end=a733_smp_counter();
    atomic_store_explicit(&a733_npu_application_result.complete,mismatch ? 2 : 1,memory_order_release);
    /* Results, requests, service, tasks and the full parent stay alive. */
    vTaskSuspend(NULL);retained(16);
}
void vApplicationStackOverflowHook(TaskHandle_t task,char *name)
{(void)task;platform_panic(name,0);}
void a733_smp_diagnostic_start(void)
{
    configASSERT(a733_smp_device_preparation_admitted());
    #ifdef SUNSTONE_NPU_SUPERVISED
    configASSERT(npu_session_copy());
#endif
    configASSERT(a733_npu_application_observer_admit());
    TaskHandle_t task=xTaskCreateStaticAffinitySet(client,"npu-check",4096,NULL,2,
        client_stack,&client_state,
#ifdef SUNSTONE_NPU_SINGLE_CORE
        1
#else
        2
#endif
    );
    configASSERT(task && a733_npu_application_init(task));
    a733_observe_service_start();
    vTaskStartScheduler();platform_panic("scheduler returned",0);
}
