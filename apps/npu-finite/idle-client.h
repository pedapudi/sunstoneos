/* Matched resident work and three bounded idle intervals. Hardware policy is
 * the sole difference between the two immutable benchmark configurations. */
#ifndef SUNSTONE_NPU_IDLE_GATE
#define SUNSTONE_NPU_IDLE_GATE 0
#endif
#if SUNSTONE_NPU_IDLE_GATE!=0 && SUNSTONE_NPU_IDLE_GATE!=1
#error "Idle gating policy must be zero or one"
#endif
#define IDLE_CYCLES 100u
__attribute__((used,retain)) const uint64_t npu_idle_return_profile[9]={
    UINT64_C(0x4e505549444c3031),1,SUNSTONE_NPU_IDLE_GATE,IDLE_CYCLES,9000,11000,100000,10000,100};
static unsigned idle_policy(void)
{return (unsigned)*(volatile const uint64_t *)&npu_idle_return_profile[2];}
static const uint64_t idle_gaps[3]={9000,11000,100000};
static uint64_t idle_warm_owner_ticks,idle_warm_owner_elapsed,idle_warm_wakes,idle_warm_irq_ticks;
static struct idle_result {
    uint64_t checked,bad,start,end,gates,resumes,writes,gated_ticks,requests,driver_max;
    uint64_t owner_ticks,owner_elapsed,client_ticks,worker_ticks,irq_ticks,idle_min,idle_max,idle_sum;
    uint64_t first_bad_cycle,first_bad_offset,first_bad_byte,pending;
    uint32_t durations[IDLE_CYCLES];uint16_t admission_us[IDLE_CYCLES];
    unsigned char output_digest[32];
} idle_results[3];
extern int npu_finite_runtime_irq_quiet(void *);
static void idle_start(void)
{
    const struct npu_runtime_queue_power_config config={
        .enabled=idle_policy(),.priority=3,.quiet=npu_finite_runtime_irq_quiet};
    if(npu_runtime_queue_power_start(&direct,&config)!=1||!direct.power_task)retained(82);
}
static unsigned idle_ready(void)
{
    return idle_results[0].checked==IDLE_CYCLES&&idle_results[1].checked==IDLE_CYCLES&&
        idle_results[2].checked==IDLE_CYCLES&&direct.power.phase==NPU_IDLE_ON&&
        atomic_load_explicit(&direct.power_stopped,memory_order_acquire)==1;
}
static unsigned idle_check(struct idle_result *r,struct storage_digest *digest,unsigned cycle)
{
    struct storage_digest one;storage_digest_init(&one);size_t at=0;unsigned bad_output=0;
    for(unsigned n=0;n<output_count;++n) {
        const unsigned char *data=outputs[n].data;
        for(size_t i=0;i<outputs[n].bytes;++i)if(data[i]!=npu_finite_networks[0].reference[at+i]) {
            bad_output=1;
            if(r->first_bad_cycle==UINT64_MAX){r->first_bad_cycle=cycle;r->first_bad_offset=at+i;r->first_bad_byte=data[i];}
        }
        if(!storage_digest_add(&one,data,outputs[n].bytes))retained(83);at+=outputs[n].bytes;
    }
    unsigned char hash[32];storage_digest_end(&one,hash);
    if(at!=npu_finite_networks[0].reference_bytes||!storage_digest_add(digest,hash,sizeof hash))retained(83);
    ++r->checked;r->bad+=bad_output;return bad_output;
}
static unsigned run_idle_cycles(void)
{
    unsigned bad_output=0;
    for(unsigned group=0;group<3;++group) {
        struct idle_result *r=&idle_results[group];struct storage_digest digest;storage_digest_init(&digest);
        r->idle_min=r->first_bad_cycle=r->first_bad_offset=r->first_bad_byte=UINT64_MAX;
        const struct npu_idle_counters prior=direct.power.counters;
        const uint64_t requests=direct.power_stats.resume_requests,irq_before=direct.irq_cpu_ticks;
        r->start=a733_smp_counter();
        const uint64_t client_before=ulTaskGetRunTimeCounter(NULL),worker_before=ulTaskGetRunTimeCounter(runtime->service.worker),
            owner_before=ulTaskGetRunTimeCounter(direct.power_task);
        for(unsigned cycle=0;cycle<IDLE_CYCLES;++cycle) {
            current_job=JOBS+group*IDLE_CYCLES+cycle;sunstone_crumb(NPU_CRUMB_SUBMIT,(unsigned)current_job);
            uint64_t lock=npu_runtime_queue_lock();
            const uint64_t full_idle=direct.power.idle_since,gates=direct.power.counters.gates,resumes=direct.power.counters.resumes;
            if(!full_idle||direct.power.phase!=NPU_IDLE_ON||direct.queue.submitted!=atomic_load_explicit(&direct.queue.completed,memory_order_acquire))retained(84);
            npu_runtime_queue_unlock(lock);
            const uint64_t until=(full_idle+23)/24+idle_gaps[group];
            while(sunstone_deadline_now_us()<until) {
                if(!live()||sunstone_deadline_wait_until(until)==SUNSTONE_DEADLINE_ERROR)retained(80);
            }
            const uint64_t before=a733_smp_counter();prepare();
            const uint64_t request=a733_smp_counter();lock=npu_runtime_queue_lock();
            const int admission=npu_runtime_queue_admit(&direct);
            npu_runtime_queue_unlock(lock);
            if(admission<0||(!admission&&npu_runtime_queue_wait_ready(&direct)!=1))retained(85);
            const uint64_t ready=a733_smp_counter(),ready_us=(ready-request+23)/24,idle_ticks=request-full_idle;
            const unsigned expected=idle_policy()&&group>0;
            lock=npu_runtime_queue_lock();
            if((admission==0)!=expected||direct.power.phase!=NPU_IDLE_ON||
               direct.power.counters.gates-gates!=expected||direct.power.counters.resumes-resumes!=expected||
               ready<request||ready_us>UINT16_MAX||(expected&&ready_us>100)||
               idle_ticks<idle_gaps[group]*24||(!group&&idle_ticks>=10000*24))retained(86);
            npu_runtime_queue_unlock(lock);r->pending+=admission==0;
            r->admission_us[cycle]=(uint16_t)ready_us;
            if(idle_ticks<r->idle_min)r->idle_min=idle_ticks;if(idle_ticks>r->idle_max)r->idle_max=idle_ticks;r->idle_sum+=idle_ticks;
            if(expected&&direct.power_stats.last_request_to_ready_us>r->driver_max)r->driver_max=direct.power_stats.last_request_to_ready_us;
            struct npu_queue_fence fence;lock=npu_runtime_queue_lock();
            const int submitted=npu_queue_submit(&direct.queue,&record,&command,(struct npu_queue_fence){0},1000000,&fence);
            npu_runtime_queue_unlock(lock);if(submitted!=1)retained(87);
            if(npu_runtime_queue_wait(&direct,fence)!=1)retained(88);
            acquire();lock=npu_runtime_queue_lock();const int released=npu_queue_release(&direct.queue,&record);
            npu_runtime_queue_unlock(lock);if(released!=1)retained(89);
            const uint64_t after=a733_smp_counter(),duration=after-before;
            if(after<=before||duration>frequency||!live())retained(90);
            r->durations[cycle]=(uint32_t)duration;bad_output|=idle_check(r,&digest,cycle);
        }
        storage_digest_end(&digest,r->output_digest);
        const uint64_t client_after=ulTaskGetRunTimeCounter(NULL),worker_after=ulTaskGetRunTimeCounter(runtime->service.worker),
            owner_after=ulTaskGetRunTimeCounter(direct.power_task);
        r->end=a733_smp_counter();
        if(client_after<client_before||worker_after<worker_before||owner_after<owner_before)retained(91);
        r->client_ticks=client_after-client_before;r->worker_ticks=worker_after-worker_before;r->owner_ticks=owner_after-owner_before;
        r->gates=direct.power.counters.gates-prior.gates;r->resumes=direct.power.counters.resumes-prior.resumes;
        r->writes=direct.power.counters.writes-prior.writes;r->gated_ticks=direct.power.counters.gated_ticks-prior.gated_ticks;
        r->owner_elapsed=direct.power.counters.owner_cpu_ticks-prior.owner_cpu_ticks;
        r->requests=direct.power_stats.resume_requests-requests;r->irq_ticks=direct.irq_cpu_ticks-irq_before;
    }
    return bad_output;
}
static void idle_report(void)
{
    const struct npu_idle_counters *c=&direct.power.counters;
    const uint64_t values[]={idle_policy(),idle_warm_owner_ticks,idle_warm_owner_elapsed,idle_warm_wakes,
        0,0,0,c->gates,c->resumes,c->writes,c->gated_ticks,c->resume_max_ticks,c->owner_cpu_ticks,c->owner_wakes,direct.irq_cpu_ticks};
    platform_puts("NPU_IDLE=\n");
    for(unsigned i=0;i<sizeof values/sizeof values[0];++i){platform_hex(values[i]);platform_puts("\n");}
    for(unsigned group=0;group<3;++group) {
        const struct idle_result *r=&idle_results[group];
        const uint64_t stats[]={idle_gaps[group],IDLE_CYCLES,r->checked,r->bad,r->start,r->end,r->gates,r->resumes,
            r->writes,r->gated_ticks,r->requests,r->driver_max,r->owner_ticks,r->owner_elapsed,r->client_ticks,r->worker_ticks,
            r->irq_ticks,r->idle_min,r->idle_max,r->idle_sum,r->first_bad_cycle,r->first_bad_offset,r->first_bad_byte,r->pending};
        platform_puts("NPU_IDLE_GROUP=\n");
        for(unsigned i=0;i<sizeof stats/sizeof stats[0];++i){platform_hex(stats[i]);platform_puts("\n");}
        platform_puts("NPU_IDLE_DURATION_LE32=\n");hex_bytes(r->durations,sizeof r->durations);
        platform_puts("NPU_IDLE_ADMISSION_LE16=\n");hex_bytes(r->admission_us,sizeof r->admission_us);
        struct storage_digest samples;storage_digest_init(&samples);unsigned char hash[32];
        if(!storage_digest_add(&samples,r->durations,sizeof r->durations)||!storage_digest_add(&samples,r->admission_us,sizeof r->admission_us))retained(83);
        storage_digest_end(&samples,hash);platform_puts("NPU_IDLE_SAMPLES_SHA256=\n");hex_bytes(hash,sizeof hash);
        platform_puts("NPU_IDLE_OUTPUT_SHA256=\n");hex_bytes(r->output_digest,sizeof r->output_digest);
    }
}
