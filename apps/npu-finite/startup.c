/* Keep the actual request, liveness, retention and task-admission helpers.
 * The unused numerical client/entry are removed by linker collection. */
#define a733_smp_diagnostic_start npu_unused_numerical_start
#include "../npu-convolution/main.c"
#undef a733_smp_diagnostic_start
#include "startup.h"
__attribute__((used,retain)) const uint64_t npu_startup_return_profile[4]={
    NPU_STARTUP_RECEIPT_MAGIC,1,0,1};
static struct npu_convolution *startup_convolution;
static unsigned startup_completed;
int npu_startup_return_ready(void)
{
    struct npu_convolution *c=startup_convolution;
    return startup_completed==NPU_STARTUP_READY && c &&
        a733_npu_application_client()==c && a733_npu_application_quiescent() &&
        atomic_load_explicit(&c->closed,memory_order_acquire) && !c->ready &&
        !c->job && !c->pending && !c->output_ready && !atomic_load_explicit(&c->failed,memory_order_acquire) &&
        c->service.controller.state==A733_NPU_CLOSED && !c->service.controller.fault &&
        !c->service.job_ticket && !c->service.has_deferred && !c->service.has_held &&
        npu_service_irq_stopped(&c->service) && npu_service_irq_completions(&c->service)==1 &&
        !a733_npu_application_result.jobs && !a733_npu_application_result.job &&
        !a733_npu_application_result.previous_job;
}
static void npu_startup_client(void *unused)
{
    (void)unused;
    platform_puts("NPU_STAGE=WAIT_STARTUP\n");
    while(!a733_npu_application_ready()) {
        if(!live())retained(9);
        vTaskDelay(1);
    }
    struct npu_convolution *c=a733_npu_application_client();
    if(!c)retained(10);
    startup_convolution=c;
    a733_npu_application_result.start=a733_smp_counter();
    platform_puts("NPU_STAGE=OPEN\n");
    struct npu_service_reply reply=request(c,NPU_SERVICE_OPEN,NULL,0,1000000);
    if(reply.state==A733_NPU_BOOTING)reply=request(c,NPU_SERVICE_WAIT,NULL,0,0);
    if(reply.state!=A733_NPU_READY || c->job || c->output_ready ||
       npu_service_irq_completions(&c->service)!=1)retained(11);
    platform_puts("NPU_STAGE=BOOTSTRAP_READY\nNPU_STAGE=CLOSE\n");
    reply=request(c,NPU_SERVICE_CLOSE,NULL,0,0);
    platform_puts("NPU_STAGE=WAIT_LOCAL_STOP\n");
    while(!a733_npu_application_stopped()) {
        if(!live())retained(18);
        vTaskDelay(1);
    }
    if(!a733_npu_application_quiesce())retained(20);
    a733_npu_application_result.end=a733_smp_counter();
    startup_completed=NPU_STARTUP_READY;
    if(!npu_startup_return_ready())retained(21);
    atomic_store_explicit(&a733_npu_application_result.complete,1,memory_order_release);
    platform_puts("NPU_IRQ_COMPLETIONS=0x0000000000000001\nNPU_IRQ_STOPPED=0x0000000000000001\n");
    platform_puts("NPU_STAGE=QUIESCENT\nNPU_STARTUP=READY_CLOSED_STOPPED; inference_jobs=0\n");
    if(!live() || !npu_startup_return_ready())retained(19);
    taskENTER_CRITICAL();platform_exit(0);
}
void a733_smp_diagnostic_start(void)
{
    configASSERT(a733_smp_device_preparation_admitted());
    configASSERT(npu_session_copy());
    configASSERT(a733_npu_application_observer_admit());
    TaskHandle_t task=xTaskCreateStaticAffinitySet(npu_startup_client,"npu-startup",4096,NULL,2,
        client_stack,&client_state,1);
    configASSERT(task && a733_npu_application_init(task));
    vTaskStartScheduler();platform_panic("scheduler returned",0);
}
