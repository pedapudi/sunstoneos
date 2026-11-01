/* Two distinct admitted models share one device-written boundary tensor.
 * Both passes prepare once and acquire after the final device command. */
__attribute__((used,retain)) const uint64_t npu_dependent_return_profile[8]={
    UINT64_C(0x4e50554445503031),SUNSTONE_NPU_CHAIN_REPEATS==1?1:2,0,0,1,0,2,SUNSTONE_NPU_CHAIN_REPEATS};
static struct npu_runtime_queue dependent_queue[2];
static struct npu_queue_command dependent_commands[2][2];
static struct npu_queue_job dependent_records[2][2];
static struct npu_runtime_tensor dependent_input,dependent_output;
static size_t dependent_cache;
static unsigned char dependent_sources[2][NPU_RUNTIME_COMMAND_BYTES] __attribute__((aligned(64)));
static struct dependent_result {
    uint64_t start,end,submitted_before_wait,checked,bad,stage,offset,value,stream_bytes;
    uint64_t repetitions,pending_checks,sum,min,max,client_ticks,worker_ticks,first_bad_repeat;
    unsigned char digest[32];
} dependent_results[2];
unsigned a733_npu_runtime_resident_slots(void){return 2;}
int a733_npu_runtime_resident_prepare(struct npu_runtime *rt)
{
    return npu_runtime_preload(rt,0,0)&&npu_runtime_preload(rt,1,1)&&
        npu_runtime_link_tensor(rt,0,0,1,0);
}
static unsigned dependent_ready(void)
{
    return !dependent_queue[0].claimed&&!dependent_queue[1].claimed&&
        dependent_results[0].checked==2*SUNSTONE_NPU_CHAIN_REPEATS&&
        dependent_results[1].checked==2*SUNSTONE_NPU_CHAIN_REPEATS&&
        atomic_load_explicit(&runtime->slot[1].state,memory_order_acquire)==NPU_RUNTIME_CLOSED;
}
static void dependent_windows(void)
{
    if(input_count!=1||output_count!=1||runtime->slots!=2||!runtime->resident||
       !npu_runtime_tensor(runtime,1,NBG_INPUT,0,&dependent_input)||
       !npu_runtime_tensor(runtime,1,NBG_OUTPUT,0,&dependent_output)||
       dependent_input.data!=outputs[0].data||dependent_input.bytes!=outputs[0].bytes||
       dependent_output.bytes!=npu_finite_networks[1].reference_bytes||
       !(dependent_cache=cache_extent(&dependent_output,&runtime->slot[1].targets[NBG_ALLOC_INPUT+1])))retained(62);
}
static unsigned dependent_check(struct dependent_result *result,struct storage_digest *digest,unsigned repetition)
{
    for(unsigned stage=0;stage<2;++stage) {
        const struct npu_runtime_tensor *out=stage?&dependent_output:&outputs[0];
        const unsigned char *data=out->data,*reference=npu_finite_networks[stage].reference;
        for(size_t at=0;at<out->bytes;++at)if(data[at]!=reference[at]) {
            result->bad=1;
            if(result->stage==UINT64_MAX){result->first_bad_repeat=repetition;result->stage=stage;result->offset=at;result->value=data[at];}
        }
        if(!storage_digest_add(digest,data,out->bytes))retained(63);
        ++result->checked;
    }
    return (unsigned)result->bad;
}
static unsigned run_dependent(void)
{
    current_job=JOBS;dependent_windows();unsigned bad_output=0;
    for(unsigned pass=0;pass<2;++pass) {
        current_job=JOBS+(pass?2*SUNSTONE_NPU_CHAIN_REPEATS:0);active_queue=&dependent_queue[pass];
        struct dependent_result *result=&dependent_results[pass];
        result->stage=result->offset=result->value=result->first_bad_repeat=result->min=UINT64_MAX;
        if(pass) {
            struct a733_npu_sequence_part parts[2];
            for(unsigned i=0;i<2;++i) {
                struct npu_runtime_slot *slot=&runtime->slot[i];
                if(!slot->stream_bytes||slot->stream_bytes>sizeof dependent_sources[i])retained(64);
                memcpy(dependent_sources[i],(const void *)slot->command.cpu,slot->stream_bytes);
                parts[i]=(struct a733_npu_sequence_part){dependent_sources[i],slot->stream_bytes};
            }
            struct npu_runtime_slot *slot=&runtime->slot[0];size_t bytes;
            if(!a733_npu_sequence_prepare((void *)slot->command.cpu,slot->command.bytes,parts,2,&bytes)||bytes>UINT32_MAX)retained(65);
            result->stream_bytes=bytes;slot->stream_bytes=(uint32_t)bytes;slot->end_mark=(uint32_t)slot->command.bytes;
            slot->span=0;slot->offset=0;slot->crc=UINT32_MAX;
            while(!npu_runtime_reference_step(runtime,slot))if(!live())retained(66);
            if(!aarch64_device_cache(AARCH64_CACHE_CLEAN,slot->command.cpu,slot->command.bytes))retained(67);
        }
        if(npu_runtime_queue_claim(active_queue,runtime)!=1)retained(68);
        const unsigned records=pass?1:2;
        for(unsigned i=0;i<records;++i)
            if(npu_queue_register(&active_queue->queue,&dependent_commands[pass][i],&runtime->commands[i+1])!=1)retained(69);
        struct storage_digest digest;storage_digest_init(&digest);
        result->start=a733_smp_counter();
        const uint64_t client_before=ulTaskGetRunTimeCounter(NULL),worker_before=ulTaskGetRunTimeCounter(runtime->service.worker);
        for(unsigned repetition=0;repetition<SUNSTONE_NPU_CHAIN_REPEATS;++repetition) {
            current_job=JOBS+(pass?2*SUNSTONE_NPU_CHAIN_REPEATS:0)+records*repetition;
            sunstone_crumb(NPU_CRUMB_SUBMIT,(unsigned)current_job);
            const uint64_t before=a733_smp_counter();prepare();poison(dependent_output.data,dependent_output.bytes);
            if(!aarch64_device_cache(AARCH64_CACHE_CLEAN_INVALIDATE,(uintptr_t)dependent_output.data,dependent_cache))retained(70);
            struct npu_queue_fence fence={0};uint64_t lock=npu_runtime_queue_lock();
            const uint64_t completed_before=atomic_load_explicit(&active_queue->queue.completed,memory_order_acquire);
            if(active_queue->queue.submitted!=completed_before)retained(72);
            for(unsigned i=0;i<records;++i)
                if(npu_queue_submit(&active_queue->queue,&dependent_records[pass][i],&dependent_commands[pass][i],fence,1000000,&fence)!=1)retained(71);
            /* IRQs remain masked until both dependent records are pending. */
            const uint64_t pending=active_queue->queue.submitted-atomic_load_explicit(&active_queue->queue.completed,memory_order_acquire);
            if(pending!=records)retained(72);
            result->submitted_before_wait=pending;++result->pending_checks;
            npu_runtime_queue_unlock(lock);
            if(npu_runtime_queue_wait(active_queue,fence)!=1)retained(73);
            acquire();
            if(!aarch64_device_cache(AARCH64_CACHE_INVALIDATE,(uintptr_t)dependent_output.data,dependent_cache))retained(74);
            lock=npu_runtime_queue_lock();
            for(unsigned i=0;i<records;++i)
                if(npu_queue_release(&active_queue->queue,&dependent_records[pass][i])!=1)retained(75);
            npu_runtime_queue_unlock(lock);
            const uint64_t after=a733_smp_counter(),duration=after-before;
            if(after<=before||duration>frequency||!live())retained(76);
            result->sum+=duration;if(duration<result->min)result->min=duration;if(duration>result->max)result->max=duration;
            bad_output|=dependent_check(result,&digest,repetition);++result->repetitions;
        }
        storage_digest_end(&digest,result->digest);
        const uint64_t client_after=ulTaskGetRunTimeCounter(NULL),worker_after=ulTaskGetRunTimeCounter(runtime->service.worker);
        if(client_after<client_before||worker_after<worker_before)retained(77);
        result->client_ticks=client_after-client_before;result->worker_ticks=worker_after-worker_before;
        result->end=a733_smp_counter();return_queue(active_queue);
    }
    return bad_output;
}
static void dependent_report(void)
{
    const struct nbg_assembly_buffer *source=&runtime->slot[0].targets[NBG_ALLOC_INPUT+1],
        *target=&runtime->slot[1].targets[NBG_ALLOC_INPUT];
    const uint64_t boundary[]={source->device,target->device,(uintptr_t)source->cpu,(uintptr_t)target->cpu,
        outputs[0].bytes,source->bytes};
    platform_puts("NPU_DEPENDENT_BOUNDARY=\n");
    for(unsigned i=0;i<sizeof boundary/sizeof boundary[0];++i){platform_hex(boundary[i]);platform_puts("\n");}
    platform_puts(SUNSTONE_NPU_CHAIN_REPEATS==1?"NPU_DEPENDENT_CHAIN=\n":"NPU_DEPENDENT_REPEATED_CHAIN=\n");
    for(unsigned pass=0;pass<2;++pass) {
        const struct dependent_result *r=&dependent_results[pass];
        const struct npu_runtime_queue *q=&dependent_queue[pass];
        const uint64_t values[]={r->start,r->end,r->submitted_before_wait,q->queue.submitted,
            atomic_load_explicit(&q->queue.completed,memory_order_acquire),r->checked,r->bad,
            r->stage,r->offset,r->value,r->stream_bytes,q->irq_cpu_ticks};
        for(unsigned i=0;i<sizeof values/sizeof values[0];++i){platform_hex(values[i]);platform_puts("\n");}
        if(SUNSTONE_NPU_CHAIN_REPEATS!=1) {
            const uint64_t repeated[]={SUNSTONE_NPU_CHAIN_REPEATS,r->repetitions,r->pending_checks,
                r->sum,r->min,r->max,r->client_ticks,r->worker_ticks,r->first_bad_repeat};
            for(unsigned i=0;i<sizeof repeated/sizeof repeated[0];++i){platform_hex(repeated[i]);platform_puts("\n");}
        }
        hex_bytes(r->digest,32);
    }
}
