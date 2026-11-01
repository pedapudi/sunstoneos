/* Matched finite latency diagnostics: legacy helper or resident queue,
 * after the same checked warmup. All measured jobs use catalogue entry 0. */
#include "../npu-convolution/native-runtime.h"
#include "npu_job.h"
#include "npu_runtime_queue.h"
#include "aarch64_device_memory.h"
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
#if !defined(SUNSTONE_NPU_LATENCY) || NPU_STARTUP_JOBS != 1001
#error "Latency diagnostics require one warmup and 1000 measured jobs"
#endif
#ifndef SUNSTONE_NPU_LATENCY_QUEUE
#define SUNSTONE_NPU_LATENCY_QUEUE 0
#endif
#define JOBS 1000u
__attribute__((used,retain)) const uint64_t npu_startup_return_profile[4]={
    NPU_STARTUP_RECEIPT_MAGIC,1,NPU_STARTUP_JOBS,1+NPU_STARTUP_JOBS};
__attribute__((used,retain)) const uint64_t npu_latency_return_profile[8]={
    UINT64_C(0x4e50554c41545031),1,SUNSTONE_NPU_LATENCY_QUEUE,JOBS,1,0,4,24000000};
struct a733_npu_application_result a733_npu_application_result;
static StaticTask_t client_state;
static StackType_t client_stack[4096] __attribute__((aligned(16)));
static struct npu_runtime *runtime;
static struct npu_runtime_queue direct;
static struct npu_queue_command command;
static struct npu_queue_job record;
static struct npu_runtime_tensor inputs[NBG_ASSEMBLY_MAX_TENSORS],outputs[NBG_ASSEMBLY_MAX_TENSORS];
static unsigned input_count,output_count,completed,checked,bad;
static unsigned char copied[NPU_FINITE_OUTPUT_BYTES];
static uint32_t durations[JOBS],ordered[JOBS];
static uint64_t frequency,start,end,sum,submit_ticks,acquire_ticks;
static uint64_t first_bad_job=UINT64_MAX,first_bad_offset=UINT64_MAX,first_bad_byte=UINT64_MAX;
static uint64_t current_job=UINT64_MAX;
/* First fault survives later refusals; breadcrumbs identify the same job. */
volatile struct npu_latency_failure {
    uint64_t reason,job,submitted,finished,events,idle;
} npu_latency_failure;
static int live(void)
{return !atomic_load_explicit(&a733_npu_application_failed,memory_order_acquire)&&a733_smp_counter()<a733_observe_identity.deadline;}
static _Noreturn void retained_at(unsigned reason,uint32_t site,const void *caller)
{
    if(!npu_latency_failure.reason)npu_latency_failure=(struct npu_latency_failure){reason,current_job,
        direct.queue.submitted,atomic_load_explicit(&direct.queue.completed,memory_order_acquire),direct.last_events,direct.last_idle};
    sunstone_crumb_fail(site,reason,caller);sunstone_crumb(NPU_CRUMB_RETAINED,(unsigned)current_job);
    a733_npu_application_fail(reason);vTaskSuspend(NULL);platform_panic("NPU latency retained",reason);
}
#define retained(reason) retained_at((reason),SUNSTONE_CRUMB_FAIL_SITE(NPU),SUNSTONE_CRUMB_HERE())
static void hex_bytes(const void *data,size_t bytes)
{
    const unsigned char *p=data;static const char hex[]="0123456789abcdef";
    for(size_t at=0;at<bytes;) {
        char line[130];size_t n=bytes-at<64?bytes-at:64;
        for(size_t i=0;i<n;++i){line[2*i]=hex[p[at+i]>>4];line[2*i+1]=hex[p[at+i]&15];}
        line[2*n]='\n';line[2*n+1]=0;platform_puts(line);at+=n;
    }
}
static void poison(void *data,size_t bytes)
{
    unsigned char *p=data;const uint32_t pattern=UINT32_C(0xc35a3ca5);
    for(size_t i=0;i<bytes;++i)p[i]=(unsigned char)(pattern>>((i&3)*8));
}
static void windows(void)
{
    size_t bytes=0;
    while(input_count<NBG_ASSEMBLY_MAX_TENSORS&&npu_runtime_tensor(runtime,0,NBG_INPUT,input_count,&inputs[input_count])) {
        if(inputs[input_count].bytes>SIZE_MAX-bytes)retained(31);
        bytes+=inputs[input_count++].bytes;
    }
    if(!input_count||bytes!=npu_finite_networks[0].input_bytes)retained(32);
    bytes=0;
    while(output_count<NBG_ASSEMBLY_MAX_TENSORS&&npu_runtime_tensor(runtime,0,NBG_OUTPUT,output_count,&outputs[output_count])) {
        if(outputs[output_count].bytes>SIZE_MAX-bytes)retained(33);
        bytes+=outputs[output_count++].bytes;
    }
    if(!output_count||bytes!=npu_finite_networks[0].reference_bytes)retained(34);
}
static void prepare(void)
{
    size_t at=0;
    for(unsigned i=0;i<input_count;++i) {
        memcpy(inputs[i].data,npu_finite_networks[0].input+at,inputs[i].bytes);at+=inputs[i].bytes;
        if(!aarch64_device_cache(AARCH64_CACHE_CLEAN,(uintptr_t)inputs[i].data,inputs[i].bytes))retained(35);
    }
    for(unsigned i=0;i<output_count;++i) {
        poison(outputs[i].data,outputs[i].bytes);
        if(!aarch64_device_cache(AARCH64_CACHE_CLEAN_INVALIDATE,(uintptr_t)outputs[i].data,outputs[i].bytes))retained(36);
    }
}
static void acquire(void)
{
    for(unsigned i=0;i<output_count;++i)
        if(!aarch64_device_cache(AARCH64_CACHE_INVALIDATE,(uintptr_t)outputs[i].data,outputs[i].bytes))retained(37);
}
static unsigned check_output(struct storage_digest *digest)
{
    const struct npu_finite_network *w=&npu_finite_networks[0];
    struct storage_digest one;storage_digest_init(&one);
    unsigned differs=0;size_t at=0;
    const unsigned count=SUNSTONE_NPU_LATENCY_QUEUE?output_count:1;
    for(unsigned n=0;n<count;++n) {
        const unsigned char *p=SUNSTONE_NPU_LATENCY_QUEUE?outputs[n].data:copied;
        size_t bytes=SUNSTONE_NPU_LATENCY_QUEUE?outputs[n].bytes:w->reference_bytes;
        for(size_t i=0;i<bytes;++i)if(p[i]!=w->reference[at+i]) {
            differs=1;
            if(first_bad_job==UINT64_MAX){first_bad_job=current_job;first_bad_offset=at+i;first_bad_byte=p[i];}
        }
        if(!storage_digest_add(&one,p,bytes))retained(38);at+=bytes;
    }
    unsigned char hash[32];storage_digest_end(&one,hash);
    if(at!=w->reference_bytes||!storage_digest_add(digest,hash,sizeof hash))retained(39);
    ++checked;bad+=differs;return differs;
}
int npu_startup_return_ready(void)
{
    const struct npu_runtime *rt=runtime;
    return completed==NPU_STARTUP_READY&&rt&&a733_npu_application_quiescent()&&
        !atomic_load_explicit(&rt->failed,memory_order_acquire)&&!direct.claimed&&
        atomic_load_explicit(&rt->slot[0].state,memory_order_acquire)==NPU_RUNTIME_CLOSED&&
        rt->slot[0].opened&&rt->slot[0].closed&&!rt->slot[0].pending&&
        rt->service.controller.state==A733_NPU_CLOSED&&!rt->service.controller.fault&&
        !rt->service.job_ticket&&!rt->service.has_deferred&&!rt->service.has_held&&
        npu_service_irq_stopped(&rt->service)&&npu_service_irq_completions(&rt->service)==1002&&
        rt->counters.jobs==1001&&checked==JOBS&&
        a733_npu_application_result.jobs==1001&&a733_npu_application_result.job==1001&&
        !a733_npu_application_result.previous_job;
}
static void report(const unsigned char output_digest[32])
{
    memcpy(ordered,durations,sizeof ordered);
    for(unsigned i=1;i<JOBS;++i){uint32_t value=ordered[i];unsigned j=i;while(j&&ordered[j-1]>value){ordered[j]=ordered[j-1];--j;}ordered[j]=value;}
    const uint64_t values[]={frequency,SUNSTONE_NPU_LATENCY_QUEUE,JOBS,1,runtime->counters.jobs,checked,bad,
        start,end,ordered[499],ordered[989],ordered[999],sum,submit_ticks,acquire_ticks,
        first_bad_job,first_bad_offset,first_bad_byte,direct.queue.submitted,
        atomic_load_explicit(&direct.queue.completed,memory_order_acquire),direct.irq_cpu_ticks};
    platform_puts("NPU_LATENCY=\n");
    for(unsigned i=0;i<sizeof values/sizeof values[0];++i){platform_hex(values[i]);platform_puts("\n");}
    platform_puts("NPU_DURATION_LE32=\n");hex_bytes(durations,sizeof durations);
    unsigned char hash[32];storage_digest_bytes(durations,sizeof durations,hash);
    platform_puts("NPU_DURATION_SHA256=\n");hex_bytes(hash,sizeof hash);
    platform_puts("NPU_OUTPUT_SHA256=\n");hex_bytes(output_digest,32);
}
static void npu_latency_client(void *unused)
{
    (void)unused;sunstone_crumb(NPU_CRUMB_WAIT_STARTUP,0);platform_puts("NPU_STAGE=WAIT_STARTUP\n");
    while(!a733_npu_application_ready()){if(!live())retained(9);vTaskDelay(1);}
    runtime=a733_npu_application_runtime();if(!runtime)retained(10);
    const struct npu_finite_network *w=&npu_finite_networks[0];
    if(!w->reference_bytes||w->reference_bytes>sizeof copied)retained(14);
    a733_npu_application_result.start=a733_smp_counter();
    __asm__ volatile("mrs %0,cntfrq_el0":"=r"(frequency));if(frequency!=24000000)retained(23);
    platform_puts("NPU_STAGE=WARMUP\n");
    if(npu_job_run(0,w->input,w->input_bytes,copied,w->reference_bytes,1000000)!=1||
       memcmp(copied,w->reference,w->reference_bytes)||!live())retained(40);
    if(SUNSTONE_NPU_LATENCY_QUEUE) {
        windows();if(npu_runtime_queue_claim(&direct,runtime)!=1)retained(41);
        uint64_t lock=npu_runtime_queue_lock();
        int registered=npu_queue_register(&direct.queue,&command,&runtime->commands[1]);
        npu_runtime_queue_unlock(lock);if(registered!=1)retained(42);
    }
    struct storage_digest digest;storage_digest_init(&digest);unsigned mismatch=0;
    platform_puts("NPU_STAGE=LATENCY\n");start=a733_smp_counter();
    for(unsigned job=0;job<JOBS;++job) {
        current_job=job;if(!live())retained(24);sunstone_crumb(NPU_CRUMB_SUBMIT,job);
        const uint64_t before=a733_smp_counter();
        if(SUNSTONE_NPU_LATENCY_QUEUE) {
            prepare();struct npu_queue_fence fence;
            uint64_t t=a733_smp_counter(),lock=npu_runtime_queue_lock();
            int submitted=npu_queue_submit(&direct.queue,&record,&command,(struct npu_queue_fence){0},1000000,&fence);
            npu_runtime_queue_unlock(lock);submit_ticks+=a733_smp_counter()-t;
            if(submitted!=1)retained(43);
            if(npu_runtime_queue_wait(&direct,fence)!=1)retained(44);
            t=a733_smp_counter();acquire();lock=npu_runtime_queue_lock();
            int released=npu_queue_release(&direct.queue,&record);
            npu_runtime_queue_unlock(lock);acquire_ticks+=a733_smp_counter()-t;
            if(released!=1)retained(45);
        }else if(npu_job_run(0,w->input,w->input_bytes,copied,w->reference_bytes,1000000)!=1)retained(12);
        const uint64_t after=a733_smp_counter(),duration=after-before;
        if(after<=before||duration>frequency||!live())retained(25);
        durations[job]=(uint32_t)duration;sum+=duration;
        mismatch|=check_output(&digest);
    }
    end=a733_smp_counter();unsigned char hash[32];storage_digest_end(&digest,hash);
    if(SUNSTONE_NPU_LATENCY_QUEUE) {
        int returned;
        do {returned=npu_runtime_queue_return(&direct);if(returned<0||!live())retained(46);if(!returned)vTaskDelay(1);}while(!returned);
    }
    if(runtime->counters.jobs!=1001||checked!=JOBS||npu_service_irq_completions(&runtime->service)!=1002)retained(47);
    const unsigned status=mismatch?1:0;
    a733_npu_application_result.jobs=1001;a733_npu_application_result.job=runtime->counters.jobs;
    sunstone_crumb(NPU_CRUMB_CLOSE,0);platform_puts("NPU_STAGE=CLOSE\n");if(npu_job_close()!=1)retained(17);
    sunstone_crumb(NPU_CRUMB_LOCAL_STOP,0);platform_puts("NPU_STAGE=WAIT_LOCAL_STOP\n");
    while(!a733_npu_application_stopped()){if(!live())retained(18);vTaskDelay(1);}
    if(!a733_npu_application_quiesce())retained(20);
    a733_npu_application_result.end=a733_smp_counter();completed=NPU_STARTUP_READY;
    if(!npu_startup_return_ready())retained(21);
    atomic_store_explicit(&a733_npu_application_result.complete,status+1,memory_order_release);
    sunstone_crumb(NPU_CRUMB_QUIESCENT,status);
    platform_puts("NPU_IRQ_COMPLETIONS=0x00000000000003ea\nNPU_IRQ_STOPPED=0x0000000000000001\nNPU_STAGE=QUIESCENT\n");
    report(hash);platform_puts(status?"NPU_INFERENCE=MISMATCH; inference_jobs=1001\n":"NPU_INFERENCE=MATCH; inference_jobs=1001\n");
    if(!live()||!npu_startup_return_ready())retained(19);
    sunstone_crumb(NPU_CRUMB_EXIT,status);taskENTER_CRITICAL();platform_exit(status);
}
void vApplicationStackOverflowHook(TaskHandle_t task,char *name)
{(void)task;platform_panic(name,0);}
void a733_smp_diagnostic_start(void)
{
    configASSERT(a733_smp_device_preparation_admitted());configASSERT(npu_session_copy());
    configASSERT(a733_npu_application_observer_admit());configASSERT(npu_finite_watchdog_start());
    struct npu_runtime_model models[NPU_FINITE_NETWORKS];
    for(unsigned n=0;n<NPU_FINITE_NETWORKS;++n)models[n]=(struct npu_runtime_model){npu_finite_networks[n].model,npu_finite_networks[n].model_bytes};
    TaskHandle_t task=xTaskCreateStaticAffinitySet(npu_latency_client,"npu-latency",4096,NULL,2,client_stack,&client_state,1);
    configASSERT(task&&a733_npu_application_runtime_init(task,models,NPU_FINITE_NETWORKS));
    vTaskStartScheduler();platform_panic("scheduler returned",0);
}
