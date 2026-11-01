/* Keep the actual request, liveness, retention and task-admission helpers.
 * The unused numerical client/entry are removed by linker collection. */
#define a733_smp_diagnostic_start npu_unused_numerical_start
#define retained npu_client_retained
#include "../npu-convolution/main.c"
#undef retained
#undef a733_smp_diagnostic_start
#include "startup.h"
#include "crumb.h"
#include "watchdog.h"
__attribute__((used,retain)) const uint64_t npu_startup_return_profile[4]={
    NPU_STARTUP_RECEIPT_MAGIC,1,NPU_STARTUP_JOBS,1+NPU_STARTUP_JOBS};
static struct npu_convolution *startup_convolution;
static unsigned startup_completed;
/* The client's last retention reason also reaches the progress record. */
static _Noreturn void retained_at(unsigned reason,uint32_t site,const void *caller)
{
    sunstone_crumb_fail(site,reason,caller);
    sunstone_crumb(NPU_CRUMB_RETAINED,reason);
    npu_client_retained(reason);
}
#define retained(reason) retained_at((reason),SUNSTONE_CRUMB_FAIL_SITE(NPU),SUNSTONE_CRUMB_HERE())
int npu_startup_return_ready(void)
{
    struct npu_convolution *c=startup_convolution;
    return startup_completed==NPU_STARTUP_READY && c &&
        a733_npu_application_client()==c && a733_npu_application_quiescent() &&
        atomic_load_explicit(&c->closed,memory_order_acquire) && !c->ready &&
        !c->job && !c->pending && !c->output_ready && !atomic_load_explicit(&c->failed,memory_order_acquire) &&
        c->service.controller.state==A733_NPU_CLOSED && !c->service.controller.fault &&
        !c->service.job_ticket && !c->service.has_deferred && !c->service.has_held &&
        npu_service_irq_stopped(&c->service) &&
        npu_service_irq_completions(&c->service)==1+NPU_STARTUP_JOBS &&
        a733_npu_application_result.jobs==NPU_STARTUP_JOBS &&
        (NPU_STARTUP_JOBS ? a733_npu_application_result.job!=0 : !a733_npu_application_result.job) &&
        !a733_npu_application_result.previous_job;
}
#if NPU_STARTUP_JOBS
/* One job on the opened service: the network is the one bound when the
 * service was constructed; the caller supplies input and output buffers.
 * Returns the job ticket after the copied output and a READY REAP. */
static uint64_t npu_job_run(struct npu_convolution *c,const void *input,size_t input_bytes,
    void *output,size_t output_bytes)
{
    memset(output,0,output_bytes);
    sunstone_crumb(NPU_CRUMB_SUBMIT,1);
    struct npu_service_reply reply=request(c,NPU_SERVICE_SUBMIT,input,input_bytes,1000000);
    uint64_t job=reply.job_ticket;
    if(!job)retained(12);
    if(reply.state==A733_NPU_RUNNING && reply.job_ticket==job)reply=request(c,NPU_SERVICE_WAIT,NULL,0,0);
    if(reply.state!=A733_NPU_DONE)retained(13);
    sunstone_crumb(NPU_CRUMB_JOB_DONE,npu_service_irq_completions(&c->service));
    if(reply.job_ticket!=job || !npu_convolution_output(c,output,output_bytes))retained(14);
    sunstone_crumb(NPU_CRUMB_OUTPUT_COPIED,(uint32_t)output_bytes);
    reply=request(c,NPU_SERVICE_REAP,NULL,0,0);
    if(reply.state!=A733_NPU_READY || c->job || c->output_ready || !live())retained(15);
    sunstone_crumb(NPU_CRUMB_REAPED,(uint32_t)job);
    return job;
}
#endif
static void npu_startup_client(void *unused)
{
    (void)unused;
    unsigned status=0;
    sunstone_crumb(NPU_CRUMB_WAIT_STARTUP,0);
    platform_puts("NPU_STAGE=WAIT_STARTUP\n");
    while(!a733_npu_application_ready()) {
        if(!live())retained(9);
        vTaskDelay(1);
    }
    struct npu_convolution *c=a733_npu_application_client();
    if(!c)retained(10);
    startup_convolution=c;
    a733_npu_application_result.start=a733_smp_counter();
    sunstone_crumb(NPU_CRUMB_OPEN,0);
    platform_puts("NPU_STAGE=OPEN\n");
    struct npu_service_reply reply=request(c,NPU_SERVICE_OPEN,NULL,0,1000000);
    if(reply.state==A733_NPU_BOOTING)reply=request(c,NPU_SERVICE_WAIT,NULL,0,0);
    if(reply.state!=A733_NPU_READY || c->job || c->output_ready ||
       npu_service_irq_completions(&c->service)!=1)retained(11);
    sunstone_crumb(NPU_CRUMB_BOOTSTRAP_READY,1);
    platform_puts("NPU_STAGE=BOOTSTRAP_READY\n");
#if NPU_STARTUP_JOBS
    platform_puts("NPU_STAGE=SUBMIT job=0x0000000000000001\n");
    const size_t bytes=a733_npu_application_reference_bytes;
    if(!bytes || bytes>sizeof(a733_npu_application_result.output))retained(14);
    uint64_t job=npu_job_run(c,a733_npu_application_input,a733_npu_application_input_bytes,
        a733_npu_application_result.output,bytes);
    /* Bootstrap plus this job; the count is unchanged by REAP. */
    if(npu_service_irq_completions(&c->service)!=2)retained(22);
    unsigned mismatch=0;
    for(size_t i=0;i<bytes;++i)
        mismatch|=a733_npu_application_result.output[i]^a733_npu_application_reference[i];
    status=mismatch?1:0;
    a733_npu_application_result.jobs=1;
    a733_npu_application_result.job=job;
    platform_puts("NPU_STAGE=REAPED\n");
#endif
    sunstone_crumb(NPU_CRUMB_CLOSE,0);
    platform_puts("NPU_STAGE=CLOSE\n");
    reply=request(c,NPU_SERVICE_CLOSE,NULL,0,0);
    sunstone_crumb(NPU_CRUMB_LOCAL_STOP,0);
    platform_puts("NPU_STAGE=WAIT_LOCAL_STOP\n");
    while(!a733_npu_application_stopped()) {
        if(!live())retained(18);
        vTaskDelay(1);
    }
    if(!a733_npu_application_quiesce())retained(20);
    a733_npu_application_result.end=a733_smp_counter();
    startup_completed=NPU_STARTUP_READY;
    if(!npu_startup_return_ready())retained(21);
    atomic_store_explicit(&a733_npu_application_result.complete,status+1,memory_order_release);
    sunstone_crumb(NPU_CRUMB_QUIESCENT,status);
#if NPU_STARTUP_JOBS
    platform_puts("NPU_IRQ_COMPLETIONS=0x0000000000000002\nNPU_IRQ_STOPPED=0x0000000000000001\n");
    platform_puts("NPU_STAGE=QUIESCENT\nNPU_OUTPUT=\n");
    for(size_t i=0;i<bytes;++i){platform_hex(a733_npu_application_result.output[i]);platform_puts("\n");}
    platform_puts(status?"NPU_INFERENCE=MISMATCH; inference_jobs=1\n":"NPU_INFERENCE=MATCH; inference_jobs=1\n");
#else
    platform_puts("NPU_IRQ_COMPLETIONS=0x0000000000000001\nNPU_IRQ_STOPPED=0x0000000000000001\n");
    platform_puts("NPU_STAGE=QUIESCENT\nNPU_STARTUP=READY_CLOSED_STOPPED; inference_jobs=0\n");
#endif
    if(!live() || !npu_startup_return_ready())retained(19);
    sunstone_crumb(NPU_CRUMB_EXIT,status);
    taskENTER_CRITICAL();platform_exit(status);
}
void a733_smp_diagnostic_start(void)
{
    configASSERT(a733_smp_device_preparation_admitted());
    configASSERT(npu_session_copy());
    configASSERT(a733_npu_application_observer_admit());
    configASSERT(npu_finite_watchdog_start());
    TaskHandle_t task=xTaskCreateStaticAffinitySet(npu_startup_client,"npu-startup",4096,NULL,2,
        client_stack,&client_state,1);
    configASSERT(task && a733_npu_application_init(task));
    vTaskStartScheduler();platform_panic("scheduler returned",0);
}
