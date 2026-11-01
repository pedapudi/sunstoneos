/* SPDX-License-Identifier: MIT */
#include "gpu_fast-app.h"
#include "gpu_queue.h"
#include "../queue-add/program.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>

#define SAMPLES 10000u
#define WARMUP 64u
struct bench_input {uint32_t magic,jobs,payload_cache,reserved;};
struct bench_result {
    uint32_t magic,version,payload_cache,jobs,frequency,serial_jobs,pipeline_jobs,irq_samples;
    uint32_t rtt[3],submit[3],irq_wait[3];
    uint32_t pipeline_ticks,pipeline_submit_ticks,depth,warmup,reserved[3];
};
_Static_assert(sizeof(struct bench_result)==96,"benchmark result");
static uint64_t counter(void)
{ uint64_t value;__asm__ volatile("isb\n mrs %0,cntvct_el0":"=r"(value)::"memory");return value; }
static void sift(uint32_t *a,unsigned root,unsigned n)
{
    for (;;) {
        unsigned child=2*root+1;
        if (child>=n) return;
        if (child+1<n && a[child]<a[child+1]) ++child;
        if (a[root]>=a[child]) return;
        uint32_t v=a[root];a[root]=a[child];a[child]=v;root=child;
    }
}
static void distribution(uint32_t *a,unsigned n,uint32_t out[3])
{
    for (unsigned i=n/2;i;i--) sift(a,i-1,n);
    for (unsigned i=n;i>1;i--) {uint32_t v=a[0];a[0]=a[i-1];a[i-1]=v;sift(a,0,i-1);}
    out[0]=a[(n+1)/2-1];out[1]=a[(99*n+99)/100-1];out[2]=a[n-1];
}
static int submit_ready(struct gpu_queue_context context,const struct gpu_queue_job *job)
{
    TickType_t began=xTaskGetTickCount();
    int r;
    while ((r=gpu_queue_submit(context,job))==GPU_QUEUE_PARKED || r==GPU_QUEUE_BUSY) {
        if (ss_live()<0 || xTaskGetTickCount()-began>=500) return r;
        ss_progress();vTaskDelay(1);
    }
    return r;
}
void gpu_fast_main(const struct ss_grant *grant)
{
    if (grant->region_count!=4 || grant->regions[GPU_FAST_REGION_CONFIG].bytes!=sizeof(struct bench_input)) ss_fail(1);
    struct bench_input input;
    memcpy(&input,grant->regions[GPU_FAST_REGION_CONFIG].base,sizeof(input));
    if (input.magic!=UINT32_C(0x31464750) || (input.jobs && input.jobs!=SAMPLES) ||
        input.payload_cache>GPU_QUEUE_PAYLOAD_UNCACHED || input.reserved) ss_fail(2);
    uint64_t frequency;
    __asm__ volatile("mrs %0,cntfrq_el0":"=r"(frequency));
    struct bench_result result={.magic=input.magic,.version=1,.payload_cache=input.payload_cache,
        .jobs=input.jobs,.frequency=(uint32_t)frequency};
    const struct gpu_queue_context_config config={.job_us=200000,.program=&gpu_queue_program,
        .lease_count=2,.lease={{.region=GPU_FAST_REGION_OUTPUT},{.region=GPU_FAST_REGION_INPUT}},
        .payload_cache=input.payload_cache};
    struct gpu_queue_context context;
    struct gpu_queue_lease_info leases[2];
    ss_progress();
    if (gpu_queue_context_create(&config,4000,&context,leases)!=GPU_QUEUE_OK) ss_fail(3);
    /* The existing vector-add shader receives count zero. Its ordinary
     * argument DMA still executes, and every lane exits before data access.
     * Both pointers select the read-only GPU lease; no output maintenance is
     * necessary for this empty workload. */
    struct gpu_queue_job job={.bindings=2,.argument_bytes=4,.groups={1,1,1},
        .binding={{.lease=1},{.lease=1}}};
    uint32_t sequence=0;
    struct gpu_queue_record record;
    if (input.jobs) {
        for (unsigned i=0;i<WARMUP;++i) {
            job.sequence=++sequence;
            if (submit_ready(context,&job)!=GPU_QUEUE_OK || gpu_queue_wait(context,&record,1000)!=GPU_QUEUE_OK ||
                record.sequence!=sequence) ss_fail(4);
        }
        result.warmup=WARMUP;
        uint32_t *rtt=grant->regions[GPU_FAST_REGION_SAMPLES].base,*submit=rtt+SAMPLES,*irq=submit+SAMPLES;
        for (unsigned i=0;i<input.jobs;++i) {
            job.sequence=++sequence;
            uint64_t began=counter();
            int r=gpu_queue_submit(context,&job);
            uint64_t kicked=counter();
            if (r!=GPU_QUEUE_OK) ss_fail(0x100u|(unsigned)r);
            if (gpu_queue_wait(context,&record,GPU_QUEUE_WAIT_FOREVER)!=GPU_QUEUE_OK || record.sequence!=sequence) ss_fail(5);
            uint64_t end=counter(),interrupt=gpu_queue_last_irq_counter(context);
            if (end-began>UINT32_MAX || kicked-began>UINT32_MAX || (interrupt && (end<interrupt || end-interrupt>UINT32_MAX))) ss_fail(6);
            rtt[i]=(uint32_t)(end-began);submit[i]=(uint32_t)(kicked-began);
            if (interrupt) irq[result.irq_samples++]=(uint32_t)(end-interrupt);
            ++result.serial_jobs;
            if (!(i&127)) ss_progress();
        }
        distribution(rtt,SAMPLES,result.rtt);distribution(submit,SAMPLES,result.submit);
        if (result.irq_samples) distribution(irq,result.irq_samples,result.irq_wait);
        struct gpu_queue_job jobs[GPU_QUEUE_DEPTH];
        struct gpu_queue_fence fences[GPU_QUEUE_DEPTH];
        for (unsigned i=0;i<GPU_QUEUE_DEPTH;++i) {jobs[i]=job;jobs[i].sequence=++sequence;}
        uint32_t accepted=0;
        uint64_t began=counter(),submission=began;
        if (gpu_queue_submit_batch(context,jobs,GPU_QUEUE_DEPTH,fences,&accepted)!=GPU_QUEUE_OK || accepted!=GPU_QUEUE_DEPTH) ss_fail(7);
        uint64_t cpu=counter()-submission;
        result.depth=accepted;
        for (unsigned i=0;i<input.jobs;++i) {
            if (gpu_queue_wait(context,&record,GPU_QUEUE_WAIT_FOREVER)!=GPU_QUEUE_OK || record.sequence!=WARMUP+SAMPLES+i+1) ss_fail(8);
            if (i+GPU_QUEUE_DEPTH<input.jobs) {
                job.sequence=++sequence;
                submission=counter();
                int r=gpu_queue_submit(context,&job);
                cpu+=counter()-submission;
                if (r!=GPU_QUEUE_OK) ss_fail(0x200u|(unsigned)r);
            }
            ++result.pipeline_jobs;
            if (!(i&127)) ss_progress();
        }
        uint64_t elapsed=counter()-began;
        if (elapsed>UINT32_MAX || cpu>UINT32_MAX) ss_fail(9);
        result.pipeline_ticks=(uint32_t)elapsed;result.pipeline_submit_ticks=(uint32_t)cpu;
    }
    ss_progress();
    if (gpu_queue_close(context,4000)!=GPU_QUEUE_OK) ss_fail(10);
    ss_finish(&result,sizeof(result));
}
