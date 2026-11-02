/* Matched resident work in three variants and three bounded idle intervals.
 * Policy 0 runs the power owner with register writes disabled (the control),
 * policy 1 gates the clocks after 10 ms of idle time, and policy 2 creates no
 * owner at all (the baseline). After the legacy 1000-job loop, five windows
 * of 12000 back-to-back jobs give repeated throughput, latency and CPU
 * samples; then cold jobs follow each idle gap (100, 100 and 30 jobs, so the
 * whole run stays well inside the 20 s diagnostic deadline). */
#include "a733_npu_idle.h"
#include "task.h"
#ifndef SUNSTONE_NPU_IDLE_GATE
#define SUNSTONE_NPU_IDLE_GATE 0
#endif
#if SUNSTONE_NPU_IDLE_GATE<0 || SUNSTONE_NPU_IDLE_GATE>2
#error "Idle policy must be zero, one or two"
#endif
#define IDLE_CYCLES 100u /* capacity of one group */
#define WINDOWS 5u
#define WINDOW_JOBS 12000u
#define HISTOGRAM 4096u
__attribute__((used,retain)) const uint64_t npu_idle_return_profile[13]={
    UINT64_C(0x4e505549444c3033),3,SUNSTONE_NPU_IDLE_GATE,100,100,30,9000,11000,100000,10000,100,WINDOWS,WINDOW_JOBS};
static unsigned idle_policy(void)
{return (unsigned)*(volatile const uint64_t *)&npu_idle_return_profile[2];}
static const uint64_t idle_gaps[3]={9000,11000,100000};
static const unsigned idle_cycles[3]={100,100,30};
/* Time left before the diagnostic deadline at the start and end of the windows and at the end of the run. */
static uint64_t idle_margin[3];
static uint64_t idle_warm_owner_ticks,idle_warm_owner_elapsed,idle_warm_wakes,idle_warm_irq_ticks;
struct output_tally {uint64_t checked,bad,first_bad,first_offset,first_byte;};
static struct window_result {
    struct output_tally output;
    uint64_t start,end,sum,p50,p99,max,overflow,client,worker,owner,idle_task,irq,wakes;
    unsigned char digest[32];
} windows_result[WINDOWS];
static struct idle_result {
    struct output_tally output;
    uint64_t start,end,gates,resumes,writes,gated_ticks,requests,driver_max;
    uint64_t owner_ticks,owner_elapsed,client_ticks,worker_ticks,idle_task,irq_ticks,idle_min,idle_max,idle_sum,pending;
    uint64_t cold_p50,cold_p99,cold_max,cold_sum,adm_p50,adm_p99,adm_max;
    uint32_t durations[IDLE_CYCLES];uint16_t admission_us[IDLE_CYCLES];
    unsigned char output_digest[32];
} idle_results[3];
static uint32_t histogram[HISTOGRAM];
extern int npu_finite_runtime_irq_quiet(void *);
static uint64_t owner_runtime(void)
{return direct.power_task?ulTaskGetRunTimeCounter(direct.power_task):0;}
static void idle_start(void)
{
    if(idle_policy()==2)return;
    /* The benchmark images retain on any resume over 100 us; production queues count it. */
    const struct npu_runtime_queue_power_config config={
        .part=&a733_npu_idle_part,.enabled=idle_policy(),.strict=1,.priority=3,.quiet=npu_finite_runtime_irq_quiet};
    if(npu_runtime_queue_power_start(&direct,&config)!=1||!direct.power_task)retained(82);
}
static unsigned idle_ready(void)
{
    unsigned done=1;
    for(unsigned i=0;i<WINDOWS;++i)done&=windows_result[i].output.checked==WINDOW_JOBS;
    for(unsigned i=0;i<3;++i)done&=idle_results[i].output.checked==idle_cycles[i];
    if(idle_policy()==2)return done;
    return done&&direct.power.phase==NPU_IDLE_ON&&atomic_load_explicit(&direct.power_stopped,memory_order_acquire)==1;
}
static void tally_start(struct output_tally *t){*t=(struct output_tally){0,0,UINT64_MAX,UINT64_MAX,UINT64_MAX};}
static unsigned idle_check(struct output_tally *r,struct storage_digest *digest,unsigned index)
{
    struct storage_digest one;storage_digest_init(&one);size_t at=0;unsigned bad_output=0;
    for(unsigned n=0;n<output_count;++n) {
        const unsigned char *data=outputs[n].data;
        for(size_t i=0;i<outputs[n].bytes;++i)if(data[i]!=npu_finite_networks[0].reference[at+i]) {
            bad_output=1;
            if(r->first_bad==UINT64_MAX){r->first_bad=index;r->first_offset=at+i;r->first_byte=data[i];}
        }
        if(!storage_digest_add(&one,data,outputs[n].bytes))retained(83);at+=outputs[n].bytes;
    }
    unsigned char hash[32];storage_digest_end(&one,hash);
    if(at!=npu_finite_networks[0].reference_bytes||!storage_digest_add(digest,hash,sizeof hash))retained(83);
    ++r->checked;r->bad+=bad_output;return bad_output;
}
/* Submit one prepared job; a suspended device reports pending and the owner
 * resumes it. Admission and submission share one raw lock span. */
static void submit_job(struct npu_queue_fence *fence,unsigned *pending)
{
    for(;;) {
        const int result=npu_runtime_queue_submit(&direct,&record,&command,(struct npu_queue_fence){0},1000000,fence);
        if(result==1)return;
        if(result<0)retained(87);
        *pending=1;
        if(npu_runtime_queue_wait_ready(&direct)!=1)retained(85);
    }
}
static void sort32(uint32_t *a,unsigned n)
{
    for(unsigned i=1;i<n;++i){uint32_t v=a[i];unsigned j=i;while(j&&a[j-1]>v){a[j]=a[j-1];--j;}a[j]=v;}
}
/* Rank-based quantile of n sorted samples: element ceil(n*percent/100). */
static uint64_t rank(unsigned n,unsigned percent){return ((uint64_t)n*percent+99)/100;}
static uint64_t margin_ticks(void)
{
    const uint64_t now=a733_smp_counter();
    return now<a733_observe_identity.deadline?a733_observe_identity.deadline-now:0;
}
static unsigned run_idle_windows(void)
{
    unsigned bad_output=0;idle_margin[0]=margin_ticks();
    for(unsigned w=0;w<WINDOWS;++w) {
        struct window_result *r=&windows_result[w];struct storage_digest digest;storage_digest_init(&digest);
        tally_start(&r->output);memset(histogram,0,sizeof histogram);
        const struct npu_idle_counters prior=direct.power.counters;const uint64_t irq_before=direct.irq_cpu_ticks;
        const uint64_t idle_task=ulTaskGetRunTimeCounter(xTaskGetIdleTaskHandle());
        const uint64_t client_before=ulTaskGetRunTimeCounter(NULL),worker_before=ulTaskGetRunTimeCounter(runtime->service.worker),
            owner_before=owner_runtime();
        r->start=a733_smp_counter();
        for(unsigned job=0;job<WINDOW_JOBS;++job) {
            current_job=JOBS+w*WINDOW_JOBS+job;if(!live())retained(24);
            const uint64_t before=a733_smp_counter();unsigned pending=0;
            prepare();struct npu_queue_fence fence;
            submit_job(&fence,&pending);
            if(npu_runtime_queue_wait(&direct,fence)!=1)retained(88);
            acquire();uint64_t lock=npu_runtime_queue_lock();const int released=npu_queue_release(&direct.queue,&record);
            npu_runtime_queue_unlock(lock);if(released!=1)retained(89);
            const uint64_t after=a733_smp_counter(),duration=after-before;
            if(after<=before||duration>frequency||pending)retained(90);
            if(duration<HISTOGRAM)++histogram[duration];else ++r->overflow;
            if(duration>r->max)r->max=duration;
            r->sum+=duration;bad_output|=idle_check(&r->output,&digest,job);
        }
        r->end=a733_smp_counter();
        const uint64_t client_after=ulTaskGetRunTimeCounter(NULL),worker_after=ulTaskGetRunTimeCounter(runtime->service.worker),
            owner_after=owner_runtime(),idle_after=ulTaskGetRunTimeCounter(xTaskGetIdleTaskHandle());
        if(client_after<client_before||worker_after<worker_before||owner_after<owner_before||idle_after<idle_task)retained(91);
        r->client=client_after-client_before;r->worker=worker_after-worker_before;r->owner=owner_after-owner_before;
        r->idle_task=idle_after-idle_task;r->irq=direct.irq_cpu_ticks-irq_before;
        r->wakes=direct.power.counters.owner_wakes-prior.owner_wakes;
        if(direct.power.counters.gates!=prior.gates||direct.power.counters.writes!=prior.writes)retained(81);
        uint64_t seen=0;const uint64_t p50=rank(WINDOW_JOBS,50),p99=rank(WINDOW_JOBS,99);
        r->p50=r->p99=HISTOGRAM;
        for(unsigned d=0;d<HISTOGRAM;++d) {
            seen+=histogram[d];
            if(r->p50==HISTOGRAM&&seen>=p50)r->p50=d;
            if(r->p99==HISTOGRAM&&seen>=p99)r->p99=d;
        }
        storage_digest_end(&digest,r->digest);
    }
    idle_margin[1]=margin_ticks();
    return bad_output;
}
static unsigned run_idle_cycles(void)
{
    unsigned bad_output=0;
    for(unsigned group=0;group<3;++group) {
        struct idle_result *r=&idle_results[group];struct storage_digest digest;storage_digest_init(&digest);
        tally_start(&r->output);r->idle_min=UINT64_MAX;
        const struct npu_idle_counters prior=direct.power.counters;
        const uint64_t requests=direct.power_stats.resume_requests,irq_before=direct.irq_cpu_ticks;
        r->start=a733_smp_counter();
        const uint64_t client_before=ulTaskGetRunTimeCounter(NULL),worker_before=ulTaskGetRunTimeCounter(runtime->service.worker),
            owner_before=owner_runtime(),idle_before=ulTaskGetRunTimeCounter(xTaskGetIdleTaskHandle());
        for(unsigned cycle=0;cycle<idle_cycles[group];++cycle) {
            current_job=JOBS+WINDOWS*WINDOW_JOBS+(group?100u:0u)+(group>1?100u:0u)+cycle;sunstone_crumb(NPU_CRUMB_SUBMIT,(unsigned)current_job);
            uint64_t lock=npu_runtime_queue_lock();
            const uint64_t full_idle=direct.power.initialized?direct.power.idle_since:a733_smp_counter(),
                gates=direct.power.counters.gates,resumes=direct.power.counters.resumes;
            if(direct.power.initialized&&(!full_idle||direct.power.phase!=NPU_IDLE_ON))retained(84);
            if(direct.queue.submitted!=atomic_load_explicit(&direct.queue.completed,memory_order_acquire))retained(84);
            npu_runtime_queue_unlock(lock);
            const uint64_t until=(full_idle+23)/24+idle_gaps[group];
            while(sunstone_deadline_now_us()<until) {
                if(!live()||sunstone_deadline_wait_until(until)==SUNSTONE_DEADLINE_ERROR)retained(80);
            }
            const uint64_t before=a733_smp_counter();prepare();
            const uint64_t request=a733_smp_counter();unsigned pending=0;
            struct npu_queue_fence fence;
            submit_job(&fence,&pending);
            const uint64_t ready=a733_smp_counter(),ready_us=(ready-request+23)/24,idle_ticks=request-full_idle;
            /* Gating is expected exactly when the device was idle for the delay; a late timer wake
             * on the 9 ms gap is a flawed measurement that the decoder reports, not a failure here. */
            const unsigned expected=idle_policy()==1&&idle_ticks>=10000*24;
            lock=npu_runtime_queue_lock();
            if(pending!=expected||(direct.power.initialized&&direct.power.phase!=NPU_IDLE_ON)||
               direct.power.counters.gates-gates!=expected||direct.power.counters.resumes-resumes!=expected||
               ready<request||ready_us>UINT16_MAX||
               idle_ticks<idle_gaps[group]*24)retained(86);
            npu_runtime_queue_unlock(lock);r->pending+=pending;
            r->admission_us[cycle]=(uint16_t)ready_us;
            if(idle_ticks<r->idle_min)r->idle_min=idle_ticks;if(idle_ticks>r->idle_max)r->idle_max=idle_ticks;r->idle_sum+=idle_ticks;
            if(expected&&direct.power_stats.last_request_to_ready_us>r->driver_max)r->driver_max=direct.power_stats.last_request_to_ready_us;
            if(npu_runtime_queue_wait(&direct,fence)!=1)retained(88);
            acquire();lock=npu_runtime_queue_lock();const int released=npu_queue_release(&direct.queue,&record);
            npu_runtime_queue_unlock(lock);if(released!=1)retained(89);
            const uint64_t after=a733_smp_counter(),duration=after-before;
            if(after<=before||duration>frequency||!live())retained(90);
            r->durations[cycle]=(uint32_t)duration;bad_output|=idle_check(&r->output,&digest,cycle);
        }
        storage_digest_end(&digest,r->output_digest);
        const uint64_t client_after=ulTaskGetRunTimeCounter(NULL),worker_after=ulTaskGetRunTimeCounter(runtime->service.worker),
            owner_after=owner_runtime(),idle_after=ulTaskGetRunTimeCounter(xTaskGetIdleTaskHandle());
        r->end=a733_smp_counter();
        if(client_after<client_before||worker_after<worker_before||owner_after<owner_before||idle_after<idle_before)retained(91);
        r->client_ticks=client_after-client_before;r->worker_ticks=worker_after-worker_before;r->owner_ticks=owner_after-owner_before;
        r->idle_task=idle_after-idle_before;
        r->gates=direct.power.counters.gates-prior.gates;r->resumes=direct.power.counters.resumes-prior.resumes;
        r->writes=direct.power.counters.writes-prior.writes;r->gated_ticks=direct.power.counters.gated_ticks-prior.gated_ticks;
        r->owner_elapsed=direct.power.counters.owner_cpu_ticks-prior.owner_cpu_ticks;
        r->requests=direct.power_stats.resume_requests-requests;r->irq_ticks=direct.irq_cpu_ticks-irq_before;
        uint32_t cold[IDLE_CYCLES];uint32_t fast[IDLE_CYCLES];
        const unsigned count=idle_cycles[group];
        for(unsigned i=0;i<count;++i){cold[i]=r->durations[i];fast[i]=r->admission_us[i];r->cold_sum+=cold[i];}
        sort32(cold,count);sort32(fast,count);
        r->cold_p50=cold[rank(count,50)-1];r->cold_p99=cold[rank(count,99)-1];r->cold_max=cold[count-1];
        r->adm_p50=fast[rank(count,50)-1];r->adm_p99=fast[rank(count,99)-1];r->adm_max=fast[count-1];
    }
    idle_margin[2]=margin_ticks();
    return bad_output;
}
static void idle_report(void)
{
    const struct npu_idle_counters *c=&direct.power.counters;
    const uint64_t values[]={idle_policy(),idle_warm_owner_ticks,idle_warm_owner_elapsed,idle_warm_wakes,
        c->gates,c->resumes,c->writes,c->gated_ticks,c->resume_max_ticks,c->resume_over_budget,
        c->owner_cpu_ticks,c->owner_wakes,direct.irq_cpu_ticks,direct.power_stats.quiet_retries,
        idle_margin[0],idle_margin[1],idle_margin[2]};
    platform_puts("NPU_IDLE=\n");
    for(unsigned i=0;i<sizeof values/sizeof values[0];++i){platform_hex(values[i]);platform_puts("\n");}
    for(unsigned w=0;w<WINDOWS;++w) {
        const struct window_result *r=&windows_result[w];
        const uint64_t stats[]={w,WINDOW_JOBS,r->output.checked,r->output.bad,r->start,r->end,r->sum,r->p50,r->p99,r->max,
            r->overflow,r->client,r->worker,r->owner,r->idle_task,r->irq,r->wakes,
            r->output.first_bad,r->output.first_offset,r->output.first_byte};
        platform_puts("NPU_IDLE_WINDOW=\n");
        for(unsigned i=0;i<sizeof stats/sizeof stats[0];++i){platform_hex(stats[i]);platform_puts("\n");}
        platform_puts("NPU_IDLE_WINDOW_OUTPUT_SHA256=\n");hex_bytes(r->digest,sizeof r->digest);
    }
    for(unsigned group=0;group<3;++group) {
        const struct idle_result *r=&idle_results[group];
        const uint64_t stats[]={idle_gaps[group],idle_cycles[group],r->output.checked,r->output.bad,r->start,r->end,r->gates,r->resumes,
            r->writes,r->gated_ticks,r->requests,r->driver_max,r->owner_ticks,r->owner_elapsed,r->client_ticks,r->worker_ticks,
            r->idle_task,r->irq_ticks,r->idle_min,r->idle_max,r->idle_sum,r->output.first_bad,r->output.first_offset,
            r->output.first_byte,r->pending,r->cold_p50,r->cold_p99,r->cold_max,r->cold_sum,r->adm_p50,r->adm_p99,r->adm_max};
        platform_puts("NPU_IDLE_GROUP=\n");
        for(unsigned i=0;i<sizeof stats/sizeof stats[0];++i){platform_hex(stats[i]);platform_puts("\n");}
        platform_puts("NPU_IDLE_OUTPUT_SHA256=\n");hex_bytes(r->output_digest,sizeof r->output_digest);
    }
}
