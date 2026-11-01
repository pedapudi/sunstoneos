/* The NPU runtime finite client. After the bootstrap activates the runtime it
 * runs one job of each linked network (networks.h) through the generic job
 * helper, npu_job_run, which opens the slot, loads each model and copies the
 * tensors. The sustained profile alternates the two models for 200 jobs.
 * It compares each copied output with the network's reference, sends
 * CLOSE, waits for the local stop, proves quiescence and returns to the ROM
 * monitor with the outputs in the platform log. Status 1 is a normal return
 * whose output differs from a reference. */
#include "../npu-convolution/native-runtime.h"
#include "npu_job.h"
#include "platform.h"
#include "boot.h"
#include "cached.h"
#include "result.h"
#include "session.h"
#include "startup.h"
#include "crumb.h"
#include "watchdog.h"
#include "networks.h"
#include <string.h>
#if !defined(SUNSTONE_NPU_RUNTIME_RETURN) || (!defined(SUNSTONE_NPU_SUSTAINED) && NPU_STARTUP_JOBS != NPU_FINITE_NETWORKS)
#error "The runtime client runs one job of each linked network"
#endif
__attribute__((used,retain)) const uint64_t npu_startup_return_profile[4]={
    NPU_STARTUP_RECEIPT_MAGIC,1,NPU_STARTUP_JOBS,1+NPU_STARTUP_JOBS};
struct a733_npu_application_result a733_npu_application_result;
static StaticTask_t client_state;
static StackType_t client_stack[4096] __attribute__((aligned(16)));
static unsigned char outputs[NPU_FINITE_NETWORKS][NPU_FINITE_OUTPUT_BYTES];
static struct npu_runtime *runtime;
#ifdef SUNSTONE_NPU_SUSTAINED
_Static_assert(NPU_STARTUP_JOBS==200 && NPU_FINITE_NETWORKS==2,"sustained plan");
/* Extends the existing runtime receipt; no new launcher execution profile. */
__attribute__((used,retain)) const uint64_t npu_sustained_return_profile[4]={
    UINT64_C(0x4e50555355533031),1,NPU_STARTUP_JOBS,NPU_FINITE_NETWORKS};
static uint64_t job_ticks[NPU_STARTUP_JOBS][2];
static uint64_t sustained_start,sustained_end,sustained_max,sustained_frequency;
static uint64_t first_bad_job=UINT64_MAX,first_bad_offset=UINT64_MAX,first_bad_byte=UINT64_MAX;
static unsigned checked,bad_outputs,model_jobs[NPU_FINITE_NETWORKS];
static unsigned char output_digest[32];
static void sustained_report(void)
{
    const uint64_t values[]={sustained_frequency,sustained_start,sustained_end,sustained_max,
        runtime->counters.jobs,checked,bad_outputs,model_jobs[0],model_jobs[1],
        first_bad_job,first_bad_offset,first_bad_byte};
    platform_puts("NPU_SUSTAINED=\n");
    for(unsigned i=0;i<sizeof values/sizeof values[0];++i){platform_hex(values[i]);platform_puts("\n");}
    platform_puts("NPU_JOB_TICKS=\n");
    for(unsigned i=0;i<NPU_STARTUP_JOBS;++i){
        platform_hex(job_ticks[i][0]);platform_puts(" ");platform_hex(job_ticks[i][1]);platform_puts("\n");
    }
    platform_puts("NPU_OUTPUT_DIGEST=\n");
    for(unsigned i=0;i<32;++i){platform_hex(output_digest[i]);platform_puts("\n");}
}
#endif
static unsigned completed;

static int live(void)
{
    return !atomic_load_explicit(&a733_npu_application_failed,memory_order_acquire) &&
        a733_smp_counter()<a733_observe_identity.deadline;
}
static _Noreturn void retained_at(unsigned reason,uint32_t site,const void *caller)
{
    sunstone_crumb_fail(site,reason,caller);
    sunstone_crumb(NPU_CRUMB_RETAINED,reason);
    a733_npu_application_fail(reason);vTaskSuspend(NULL);
    platform_panic("NPU client retained",reason);
}
#define retained(reason) retained_at((reason),SUNSTONE_CRUMB_FAIL_SITE(NPU),SUNSTONE_CRUMB_HERE())
int npu_startup_return_ready(void)
{
    const struct npu_runtime *rt=runtime;
    return completed==NPU_STARTUP_READY && rt && a733_npu_application_quiescent() &&
        !atomic_load_explicit(&rt->failed,memory_order_acquire) &&
        atomic_load_explicit(&rt->slot[0].state,memory_order_acquire)==NPU_RUNTIME_CLOSED &&
        rt->slot[0].opened && rt->slot[0].closed && !rt->slot[0].pending &&
        rt->service.controller.state==A733_NPU_CLOSED && !rt->service.controller.fault &&
        !rt->service.job_ticket && !rt->service.has_deferred && !rt->service.has_held &&
        npu_service_irq_stopped(&rt->service) &&
        npu_service_irq_completions(&rt->service)==1+NPU_STARTUP_JOBS &&
        rt->counters.jobs==NPU_STARTUP_JOBS &&
        a733_npu_application_result.jobs==NPU_STARTUP_JOBS &&
        a733_npu_application_result.job==NPU_STARTUP_JOBS && !a733_npu_application_result.previous_job;
}
#ifndef SUNSTONE_NPU_SUSTAINED
static void hex_lines(const unsigned char *bytes,size_t count)
{
#ifdef SUNSTONE_NPU_MODEL_SWITCH
    static const char hex[]="0123456789abcdef";
    for(size_t at=0;at<count;) {
        char line[130];size_t n=count-at<64?count-at:64;
        for(size_t i=0;i<n;++i){line[2*i]=hex[bytes[at+i]>>4];line[2*i+1]=hex[bytes[at+i]&15];}
        line[2*n]='\n';line[2*n+1]=0;platform_puts(line);at+=n;
    }
#else
    for(size_t i=0;i<count;++i){platform_hex(bytes[i]);platform_puts("\n");}
#endif
}
#endif
static void npu_runtime_client(void *unused)
{
    (void)unused;
    sunstone_crumb(NPU_CRUMB_WAIT_STARTUP,0);
    platform_puts("NPU_STAGE=WAIT_STARTUP\n");
    while(!a733_npu_application_ready()) {
        if(!live())retained(9);
        vTaskDelay(1);
    }
    runtime=a733_npu_application_runtime();
    if(!runtime)retained(10);
    a733_npu_application_result.start=a733_smp_counter();
    unsigned mismatch=0;
#ifdef SUNSTONE_NPU_SUSTAINED
    struct storage_digest digest;
    storage_digest_init(&digest);
    __asm__ volatile("mrs %0,cntfrq_el0":"=r"(sustained_frequency));
    if(sustained_frequency!=24000000)retained(23);
    platform_puts("NPU_STAGE=SUSTAINED\n");
    for(unsigned job=0;job<NPU_STARTUP_JOBS;++job) {
        const unsigned n=job%NPU_FINITE_NETWORKS;
#else
    for(unsigned n=0;n<NPU_FINITE_NETWORKS;++n) {
#endif
        const struct npu_finite_network *w=&npu_finite_networks[n];
        if(!w->reference_bytes || w->reference_bytes>NPU_FINITE_OUTPUT_BYTES)retained(14);
#ifndef SUNSTONE_NPU_SUSTAINED
        platform_puts("NPU_STAGE=JOB network=");platform_hex(n);platform_puts("\n");
#endif
        memset(outputs[n],0,w->reference_bytes);
        /* Model index n: the runtime was prepared with the networks in order. */
#ifdef SUNSTONE_NPU_SUSTAINED
        if(!live())retained(24);
        job_ticks[job][0]=a733_smp_counter();
        if(!job)sustained_start=job_ticks[job][0];
#endif
#ifdef SUNSTONE_NPU_SUSTAINED
        const int result=npu_job_run(n,w->input,w->input_bytes,outputs[n],w->reference_bytes,1000000);
        job_ticks[job][1]=a733_smp_counter();
        if(result!=1 || !live())retained(12+n);
        /* Qualification budget: complete the entire helper API in one second,
         * including LOAD and copies, within the existing global 20 s bound. */
        if(job_ticks[job][1]<=job_ticks[job][0] ||
           job_ticks[job][1]-job_ticks[job][0]>sustained_frequency)retained(25);
        const uint64_t duration=job_ticks[job][1]-job_ticks[job][0];
        if(duration>sustained_max)sustained_max=duration;
#else
        if(npu_job_run(n,w->input,w->input_bytes,outputs[n],w->reference_bytes,1000000)!=1 || !live())retained(12+n);
#endif
        const unsigned differs=memcmp(outputs[n],w->reference,w->reference_bytes)!=0;
        mismatch|=differs;
#ifdef SUNSTONE_NPU_SUSTAINED
        ++checked;++model_jobs[n];bad_outputs+=differs;
        if(differs && first_bad_job==UINT64_MAX) {
            first_bad_job=job;
            for(size_t i=0;i<w->reference_bytes;++i)if(outputs[n][i]!=w->reference[i]) {
                first_bad_offset=i;first_bad_byte=outputs[n][i];break;
            }
        }
        unsigned char one[32];storage_digest_bytes(outputs[n],w->reference_bytes,one);
        if(!storage_digest_add(&digest,one,sizeof one))retained(26);
        if(!live())retained(27);
#endif
    }
#ifdef SUNSTONE_NPU_SUSTAINED
    sustained_end=a733_smp_counter();
    storage_digest_end(&digest,output_digest);
    if(checked!=NPU_STARTUP_JOBS || runtime->counters.jobs!=NPU_STARTUP_JOBS ||
       model_jobs[0]!=100 || model_jobs[1]!=100 || !live())retained(28);
#endif
    if(npu_service_irq_completions(&runtime->service)!=1+NPU_STARTUP_JOBS)retained(22);
    const unsigned status=mismatch?1:0;
    a733_npu_application_result.jobs=NPU_STARTUP_JOBS;
    a733_npu_application_result.job=(uint64_t)runtime->counters.jobs;
    sunstone_crumb(NPU_CRUMB_CLOSE,0);
    platform_puts("NPU_STAGE=CLOSE\n");
    if(npu_job_close()!=1)retained(17);
    sunstone_crumb(NPU_CRUMB_LOCAL_STOP,0);
    platform_puts("NPU_STAGE=WAIT_LOCAL_STOP\n");
    while(!a733_npu_application_stopped()) {
        if(!live())retained(18);
        vTaskDelay(1);
    }
    if(!a733_npu_application_quiesce())retained(20);
    a733_npu_application_result.end=a733_smp_counter();
    completed=NPU_STARTUP_READY;
    if(!npu_startup_return_ready())retained(21);
    atomic_store_explicit(&a733_npu_application_result.complete,status+1,memory_order_release);
    sunstone_crumb(NPU_CRUMB_QUIESCENT,status);
    platform_puts("NPU_IRQ_COMPLETIONS=");platform_hex(npu_service_irq_completions(&runtime->service));
    platform_puts("\nNPU_IRQ_STOPPED=0x0000000000000001\nNPU_STAGE=QUIESCENT\n");
#ifdef SUNSTONE_NPU_SUSTAINED
    sustained_report();
#else
    for(unsigned n=0;n<NPU_FINITE_NETWORKS;++n) {
        platform_puts(
#ifdef SUNSTONE_NPU_MODEL_SWITCH
            "NPU_OUTPUT_HEX network="
#else
            "NPU_OUTPUT network="
#endif
        );platform_hex(n);platform_puts("\n");
        hex_lines(outputs[n],npu_finite_networks[n].reference_bytes);
    }
#endif
    platform_puts(status?"NPU_INFERENCE=MISMATCH; inference_jobs=" NPU_STARTUP_JOB_TEXT "\n":
                        "NPU_INFERENCE=MATCH; inference_jobs=" NPU_STARTUP_JOB_TEXT "\n");
    if(!live() || !npu_startup_return_ready())retained(19);
    sunstone_crumb(NPU_CRUMB_EXIT,status);
    taskENTER_CRITICAL();platform_exit(status);
}
void vApplicationStackOverflowHook(TaskHandle_t task,char *name)
{(void)task;platform_panic(name,0);}
void a733_smp_diagnostic_start(void)
{
    configASSERT(a733_smp_device_preparation_admitted());
    configASSERT(npu_session_copy());
    configASSERT(a733_npu_application_observer_admit());
    configASSERT(npu_finite_watchdog_start());
    struct npu_runtime_model models[NPU_FINITE_NETWORKS];
    for(unsigned n=0;n<NPU_FINITE_NETWORKS;++n)
        models[n]=(struct npu_runtime_model){npu_finite_networks[n].model,npu_finite_networks[n].model_bytes};
    TaskHandle_t task=xTaskCreateStaticAffinitySet(npu_runtime_client,"npu-runtime",4096,NULL,2,
        client_stack,&client_state,1);
    configASSERT(task && a733_npu_application_runtime_init(task,models,NPU_FINITE_NETWORKS));
    vTaskStartScheduler();platform_panic("scheduler returned",0);
}
