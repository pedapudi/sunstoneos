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
#ifdef SUNSTONE_NPU_ACTIVE_CANCEL
#include "npu_convolution_layout.h"
__attribute__((used,retain)) const uint64_t npu_active_cancel_profile[6]={
    UINT64_C(0x4e505543414e3031),NPU_CONVOLUTION_WAIT_PAIRS,NPU_CONVOLUTION_WAIT_BYTES,
    NPU_CONVOLUTION_COMMAND_BYTES,NPU_CONVOLUTION_RESERVED_BYTES,UINT32_C(0xe010a000)};
#endif
__attribute__((used,retain)) const uint64_t npu_startup_return_profile[4]={
    NPU_STARTUP_RECEIPT_MAGIC,1,NPU_STARTUP_JOBS,NPU_STARTUP_COMPLETIONS};
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
        npu_service_irq_completions(&c->service)==NPU_STARTUP_COMPLETIONS &&
#ifdef SUNSTONE_NPU_ACTIVE_CANCEL
        c->diagnostic_reaped && c->diagnostic_recovered && !c->diagnostic_recovering &&
        c->diagnostic.recovered && atomic_load_explicit(&c->diagnostic.irq_receipts,memory_order_acquire)==3 &&
#endif
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
    struct npu_service_reply reply=request(c,NPU_SERVICE_SUBMIT,input,input_bytes,
#ifdef SUNSTONE_NPU_ACTIVE_CANCEL
        100000
#else
        1000000
#endif
    );
    uint64_t job=reply.job_ticket;
    if(!job)retained(12);
    if(reply.state==A733_NPU_RUNNING && reply.job_ticket==job)reply=request(c,NPU_SERVICE_WAIT,NULL,0,0);
    if(reply.state!=A733_NPU_DONE)retained(13);
    sunstone_crumb(NPU_CRUMB_JOB_DONE,npu_service_irq_completions(&c->service));
    if(reply.job_ticket!=job || !npu_convolution_output(c,output,output_bytes))retained(14);
#ifdef SUNSTONE_NPU_ACTIVE_CANCEL
    if(output_bytes!=a733_npu_application_reference_bytes) {
        sunstone_crumb_fail(SUNSTONE_CRUMB_FAIL_SITE(NPU)|SUNSTONE_CRUMB_NPU_OUTPUT|((uint32_t)(job&511)<<16),
            SUNSTONE_CRUMB_NPU_OUTPUT_LENGTH|(uint32_t)output_bytes,SUNSTONE_CRUMB_HERE());retained(34);
    }
    for(size_t i=0;i<output_bytes;++i)if(((unsigned char *)output)[i]!=a733_npu_application_reference[i]) {
        sunstone_crumb_fail(SUNSTONE_CRUMB_FAIL_SITE(NPU)|SUNSTONE_CRUMB_NPU_OUTPUT|((uint32_t)(job&511)<<16),
            ((uint32_t)i<<8)|((unsigned char *)output)[i],SUNSTONE_CRUMB_HERE());retained(34);
    }
    sunstone_crumb(SUNSTONE_CRUMB_NPU_CANCEL_OUTPUT,(uint32_t)job);
#endif
    sunstone_crumb(NPU_CRUMB_OUTPUT_COPIED,(uint32_t)output_bytes);
    reply=request(c,NPU_SERVICE_REAP,NULL,0,0);
    if(reply.state!=A733_NPU_READY || c->job || c->output_ready || !live())retained(15);
    sunstone_crumb(NPU_CRUMB_REAPED,(uint32_t)job);
    return job;
}
#endif
#ifdef SUNSTONE_NPU_ACTIVE_CANCEL
static struct npu_service_reply cancel_collect(struct npu_convolution *c,uint64_t ticket,
    enum npu_service_operation operation)
{
    struct npu_service_reply reply;
    for(;;) {
        if(!live())retained(25);
        if(npu_convolution_collect(c,ticket,&reply,1))break;
    }
    if(atomic_load_explicit(&c->failed,memory_order_acquire) || reply.result!=NPU_SERVICE_OK ||
       reply.fault || !reply.owns_session ||
       (reply.canceled && (operation!=NPU_SERVICE_REAP || reply.state!=A733_NPU_DRAINED)))retained(26);
    return reply;
}
static struct npu_service_reply cancel_request(struct npu_convolution *c,enum npu_service_operation operation,uint64_t timeout)
{
    uint64_t ticket;
    if(!live() || !npu_convolution_send(c,operation,NULL,0,timeout,1,&ticket))retained(27);
    return cancel_collect(c,ticket,operation);
}
static void cancel_and_recover(struct npu_convolution *c,size_t bytes)
{
    uint64_t ticket;
    if(!npu_convolution_diagnostic_send(c,a733_npu_application_input,a733_npu_application_input_bytes,
        100000,1,&ticket))retained(28);
    struct npu_service_reply reply=cancel_collect(c,ticket,NPU_SERVICE_SUBMIT);
    if(reply.state!=A733_NPU_RUNNING || reply.job_ticket!=ticket)retained(29);
    reply=cancel_request(c,NPU_SERVICE_CANCEL,20000);
    if(reply.state==A733_NPU_CANCEL_ASSERTED || reply.state==A733_NPU_CANCEL_WAIT)
        reply=cancel_request(c,NPU_SERVICE_WAIT,0);
    if(reply.state!=A733_NPU_CANCELED || reply.job_ticket!=ticket ||
       !npu_convolution_output(c,a733_npu_application_result.output,bytes))retained(30);
    for(size_t i=0;i<bytes;++i)if(a733_npu_application_result.output[i]!=(unsigned char)
        (NPU_CONVOLUTION_OUTPUT_POISON>>8*(i&3))) {
        sunstone_crumb_fail(SUNSTONE_CRUMB_FAIL_SITE(NPU)|SUNSTONE_CRUMB_NPU_OUTPUT|((uint32_t)(ticket&511)<<16),
            ((uint32_t)i<<8)|a733_npu_application_result.output[i],SUNSTONE_CRUMB_HERE());retained(31);
    }
    reply=cancel_request(c,NPU_SERVICE_REAP,0);
    if(reply.state!=A733_NPU_DRAINED || !reply.canceled || reply.job_ticket!=ticket ||
       c->job || c->output_ready || c->ready)retained(32);
    reply=cancel_request(c,NPU_SERVICE_RECOVER,100000);
    if(reply.state==A733_NPU_BOOTING)reply=cancel_request(c,NPU_SERVICE_WAIT,0);
    if(reply.state!=A733_NPU_READY || npu_service_irq_completions(&c->service)!=2 ||
       !c->diagnostic_recovered || !c->diagnostic.recovered)retained(33);
    platform_puts("NPU_STAGE=CANCELED_RECOVERED\n");
}
static void cancel_word(uint64_t word){platform_hex(word);platform_puts("\n");}
static void cancel_report(const struct npu_convolution *c)
{
    const struct npu_service_cancel_diagnostic *d=&c->diagnostic;
    extern uint32_t npu_active_cancel_platform[2][6];
    extern unsigned npu_active_cancel_authority[2],npu_active_cancel_checkpoints;
    extern uint64_t npu_active_cancel_platform_ticks[2];
    platform_puts("NPU_CANCEL_RECEIPT=\n");
    cancel_word(d->job_ticket);cancel_word(d->trigger_us);cancel_word(d->cancel_asserted_us);
    cancel_word(d->cancel_released_us);cancel_word(d->cancel_done_us);cancel_word(d->recovery_ticket);
    cancel_word(atomic_load_explicit(&d->irq_receipts,memory_order_acquire));cancel_word(d->recovery_receipts);
    cancel_word(d->cancel_status);cancel_word(d->cancel_idle);
    for(unsigned i=0;i<3;++i) {
        const struct npu_service_activity_sample *s=&d->samples[i];
        cancel_word(s->time_us);cancel_word(s->job_ticket);cancel_word(s->idle);
        cancel_word(s->frontend[0]);cancel_word(s->frontend[1]);cancel_word(s->fetch[0]);cancel_word(s->fetch[1]);cancel_word(s->state);
    }
    for(unsigned stage=0;stage<2;++stage) {
        for(unsigned i=0;i<6;++i)cancel_word(npu_active_cancel_platform[stage][i]);
        cancel_word(npu_active_cancel_authority[stage]);cancel_word(npu_active_cancel_platform_ticks[stage]);
    }
    cancel_word(npu_active_cancel_checkpoints);
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
    const size_t bytes=a733_npu_application_reference_bytes;
    if(!bytes || bytes>sizeof(a733_npu_application_result.output))retained(14);
#ifdef SUNSTONE_NPU_ACTIVE_CANCEL
    cancel_and_recover(c,bytes);
#endif
    platform_puts("NPU_STAGE=SUBMIT job=0x0000000000000001\n");
    uint64_t job=npu_job_run(c,a733_npu_application_input,a733_npu_application_input_bytes,
        a733_npu_application_result.output,bytes);
    /* Bootstrap plus this job; the count is unchanged by REAP. */
    if(npu_service_irq_completions(&c->service)!=NPU_STARTUP_COMPLETIONS)retained(22);
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
#ifdef SUNSTONE_NPU_ACTIVE_CANCEL
    sunstone_crumb(SUNSTONE_CRUMB_NPU_CANCEL_QUIESCENT,3);
#endif
    atomic_store_explicit(&a733_npu_application_result.complete,status+1,memory_order_release);
    sunstone_crumb(NPU_CRUMB_QUIESCENT,status);
#if NPU_STARTUP_JOBS
#ifdef SUNSTONE_NPU_ACTIVE_CANCEL
    platform_puts("NPU_IRQ_COMPLETIONS=0x0000000000000003\nNPU_IRQ_STOPPED=0x0000000000000001\n");
    cancel_report(c);
#else
    platform_puts("NPU_IRQ_COMPLETIONS=0x0000000000000002\nNPU_IRQ_STOPPED=0x0000000000000001\n");
#endif
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
