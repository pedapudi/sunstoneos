/* Firmware timing model for the actual EL0 public API, EL1 caller transport,
 * retained native memory and GIC interrupt path. No GPU shader is executed. */
struct fast_model_job {
    uint32_t serial, cccb, kccb;
    unsigned char *page;
    unsigned char snapshot[PVR_COMPUTE_FAST_STRIDE];
    uint64_t doorbell;
};
static struct fast_model_job fast_jobs[PVR_COMPUTE_FAST_DEPTH];
static atomic_uint fast_produced,fast_consumed;
static atomic_uint fast_unlock_request;
static unsigned fast_recovery_contention;
static unsigned fast_recovery_interleave;
void __real_pvr_compute_fast_unlock(struct pvr_compute_queue *);
void __wrap_pvr_compute_fast_unlock(struct pvr_compute_queue *q)
{
    if (gpu_queue_case==56 && !fast_recovery_interleave && xTaskGetCurrentTaskHandle()==service.worker &&
        atomic_load(&q->fast.fault_through[0])) {
        fast_recovery_interleave=1;
        /* Fixture-only preemption point: preserve the transport lock while
         * the notified owner observes BUSY and registers its second wait. */
        vTaskDelay(1);
    }
    __real_pvr_compute_fast_unlock(q);
}
int __real_pvr_compute_fast_collect(struct pvr_compute_queue *,unsigned,uint32_t,uint32_t,struct gpu_queue_record *);
int __wrap_pvr_compute_fast_collect(struct pvr_compute_queue *q,unsigned client,uint32_t serial,uint32_t sequence,
    struct gpu_queue_record *record)
{
    if (gpu_queue_case==56 && atomic_load(&q->fast.locked) && atomic_load(&q->fast.fault_through[client]))
        ++fast_recovery_contention;
    return __real_pvr_compute_fast_collect(q,client,serial,sequence,record);
}
static int fast_kick(void *ctx,unsigned offset,unsigned value,unsigned char *command)
{
    configASSERT(ctx==&config && offset==0xb00 && !value && running && level()==4);
    uint32_t cursor=(uint32_t)word(command+12,4);
    configASSERT(cursor>=184 && cursor<=FIXTURE_RING_BYTES);
    unsigned char *ring=(unsigned char *)compute->firmware[3].storage.cpu+cursor-184;
    configASSERT(word(ring,4)==(PVR_CCB_MAGIC|PVR_CCCB_TASK|205u) && word(ring+4,4)==96 &&
        word(ring+136,4)==(PVR_CCB_MAGIC|213u) && word(ring+176,4)==compute->firmware_address[5]);
    uint32_t serial=(uint32_t)word(ring+180,4);
    uint64_t cdm=word(ring+80,8);
    configASSERT(cdm>=compute->job_gpu+PVR_COMPUTE_QUEUE_CDM);
    uint64_t slot=cdm-compute->job_gpu-PVR_COMPUTE_QUEUE_CDM;
    configASSERT(!(slot%PVR_COMPUTE_FAST_STRIDE) && slot<PVR_COMPUTE_FAST_DEPTH*PVR_COMPUTE_FAST_STRIDE);
    unsigned produced=atomic_load(&fast_produced);
    configASSERT(produced-atomic_load(&fast_consumed)<PVR_COMPUTE_FAST_DEPTH && serial==produced+1);
    struct fast_model_job *j=&fast_jobs[produced%PVR_COMPUTE_FAST_DEPTH];
    *j=(struct fast_model_job){.serial=serial,.cccb=cursor,.kccb=(uint32_t)word(resource(0),4),
        .page=compute->job_cpu+slot,.doorbell=now_us()};
    memcpy(j->snapshot,j->page,sizeof(j->snapshot));
    atomic_store_explicit(&fast_produced,produced+1,memory_order_release);
    return gpu_queue_case!=61;
}
static void fast_model(void *unused)
{
    (void)unused;
    for (;;) {
        if (atomic_exchange(&fast_unlock_request,0)) {
            uint64_t deadline=now_us()+100000;
            while (!atomic_load(&compute->fast.lock_waiter[0])) configASSERT(now_us()<deadline);
            pvr_compute_fast_unlock(compute);
        }
        unsigned consumed=atomic_load_explicit(&fast_consumed,memory_order_relaxed);
        if (consumed==atomic_load_explicit(&fast_produced,memory_order_acquire)) continue;
        struct fast_model_job *j=&fast_jobs[consumed%PVR_COMPUTE_FAST_DEPTH];
        /* Model firmware command acquisition 3 us after the doorbell. Old
         * read offsets remain visible until that acquisition completes. */
        while (now_us()<j->doorbell+3) {}
        put(compute->firmware[2].storage.cpu,4,j->cccb);
        put((unsigned char *)compute->firmware[2].storage.cpu+4,4,j->cccb%FIXTURE_RING_BYTES);
        set((uint32_t *)(resource(0)+128),j->kccb);
        fixture_executed();
        /* Execution and publication take a further 17 us. The payload must
         * stay unchanged through completion, including pipelined jobs. */
        while (now_us()<j->doorbell+20 || atomic_load(&hold_completion)) {}
        configASSERT(!memcmp(j->page,j->snapshot,sizeof(j->snapshot)));
        uint64_t output=word(j->page+96,8);
        uint32_t argument=(uint32_t)word(j->page+PVR_COMPUTE_QUEUE_ARGUMENTS+8,4);
        configASSERT(output>=FIXTURE_OUTPUT && output+4<=FIXTURE_OUTPUT+FIXTURE_OUTPUT_BYTES);
        put((void *)(uintptr_t)output,4,argument);
        __asm__ volatile("dsb sy" ::: "memory");
        set(compute->firmware[5].storage.cpu,j->serial+(gpu_queue_case==58));
        /* Model 2 us from timeline publication to level interrupt assertion. */
        uint64_t published=now_us();
        while (now_us()<published+2) {}
        atomic_store_explicit(&fast_consumed,consumed+1,memory_order_release);
        if (gpu_queue_case==56 && j->serial==1) continue;
        set(&gpu_status,1);
        *(volatile uint32_t *)(uintptr_t)(GICD+0x200+4*(95/32))=1u<<(95%32);
        __asm__ volatile("dsb sy" ::: "memory");
    }
}
static struct gpu_queue_job fast_job(unsigned sequence,unsigned slot)
{
    struct gpu_queue_job job={.sequence=sequence,.bindings=2,.argument_bytes=4,.groups={1,1,1}};
    job.binding[0]=(struct gpu_queue_binding){.lease=0,.address=slot*128,.sync_offset=slot*128,.sync_bytes=64};
    job.binding[1]=(struct gpu_queue_binding){.lease=3,.address=slot*64,.sync_offset=slot*64,.sync_bytes=64};
    memcpy(job.arguments,&sequence,4);
    return job;
}
static void fast_cases(unsigned c)
{
    configASSERT(c>=54 && c<=62 && level()==0);
    while (service.irq_masked) vTaskDelay(1);
    if (c==56 || c==58) vTaskPrioritySet(NULL,5);
    else vTaskSuspend(service.worker);
    unsigned requests=uxQueueMessagesWaiting(service.requests);
    if (c==60) {
        atomic_store(&compute->fast.locked,1);
        atomic_store(&fast_unlock_request,1);
        struct gpu_queue_fence invalid={context,123,123};
        struct gpu_queue_record record;
        memset(&record,0xa5,sizeof(record));
        configASSERT(gpu_queue_wait_fence(invalid,&record,GPU_QUEUE_WAIT_FOREVER)==GPU_QUEUE_REFUSED &&
            record.sequence==UINT32_C(0xa5a5a5a5) && !compute->fast.used && !irq_count);
        platform_puts("PASS GPU queue case ");platform_hex(c);
        platform_puts("; unvalidated future fence waits only for the lock and refuses without a GPU interrupt\n");platform_exit(0);
    }
    if (c==61) {
        struct gpu_queue_job jobs[2]={fast_job(1,0),fast_job(2,1)};
        struct gpu_queue_fence fences[2]={0};
        uint32_t accepted=0;
        configASSERT(gpu_queue_submit_batch(context,jobs,2,fences,&accepted)==GPU_QUEUE_FAULT && accepted==1 &&
            fences[0].serial==1 && fences[0].sequence==1 && !fences[1].serial && compute->fast.used==1 &&
            atomic_load(&compute->client_owed)==1);
        platform_puts("PASS GPU queue case ");platform_hex(c);
        platform_puts("; an uncertain doorbell retains and reports the attempted batch prefix\n");platform_exit(0);
    }
    if (c==56 || c==58) {
        struct gpu_queue_job job=fast_job(1,0);
        configASSERT(gpu_queue_submit(context,&job)==GPU_QUEUE_OK);
        struct gpu_queue_record record;
        memset(&record,0xa5,sizeof(record));
        configASSERT(gpu_queue_wait(context,&record,GPU_QUEUE_WAIT_FOREVER)==GPU_QUEUE_FAULT);
        if (c==58) {
            configASSERT(record.sequence==UINT32_C(0xa5a5a5a5) && compute->fast.used==1 &&
                atomic_load(&compute->client_owed)==1 && atomic_load(&compute->lost) &&
                compute->fast.fault_observed==2 && compute->fast.fault_submitted==1);
        } else {
            if (!(record.sequence==1 && record.detail==GPU_QUEUE_HANG && !compute->fast.used &&
                    !atomic_load(&compute->client_owed) && fast_recovery_contention && reset_readbacks)) {
                metric("fault-sequence",record.sequence);metric("fault-detail",record.detail);
                metric("retained-slots",compute->fast.used);metric("owed",atomic_load(&compute->client_owed));
                metric("recovery-contention",fast_recovery_contention);metric("reset-readbacks",reset_readbacks);
                metric("service-phase",service.phase);metric("service-error",service.error);metric("queue-error",compute->error);
                metric("irq-error",atomic_load(&compute->fast.irq_error));metric("fault-line",compute->fast.fault_line);
            }
            configASSERT(record.sequence==1 && record.detail==GPU_QUEUE_HANG && !compute->fast.used &&
                !atomic_load(&compute->client_owed) && fast_recovery_contention && reset_readbacks);
            configASSERT(gpu_queue_recover(context,1000)==GPU_QUEUE_OK);
            job=fast_job(2,0);
            configASSERT(gpu_queue_submit(context,&job)==GPU_QUEUE_OK &&
                gpu_queue_wait(context,&record,GPU_QUEUE_WAIT_FOREVER)==GPU_QUEUE_OK && record.sequence==2);
        }
        platform_puts("PASS GPU queue case ");platform_hex(c);
        platform_puts(c==56?"; missing IRQ faults after deadline; isolated fault survives lock contention and restart\n":
            "; permanent fault wakes an infinite waiter while retaining the owed slot and output\n");
        platform_exit(0);
    }
    if (c==57) {
        atomic_store(&hold_completion,1);
        struct gpu_queue_job job=fast_job(1,0);
        struct gpu_queue_fence fence;
        configASSERT(gpu_queue_submit_fence(context,&job,&fence)==GPU_QUEUE_OK);
        struct gpu_queue_record record;
        memset(&record,0xa5,sizeof(record));
        configASSERT(gpu_queue_wait_fence(fence,&record,0)==GPU_QUEUE_TIMEOUT &&
            record.sequence==UINT32_C(0xa5a5a5a5) && compute->fast.used==1);
        job.sequence=2;
        configASSERT(gpu_queue_submit(context,&job)==GPU_QUEUE_BUSY);
        atomic_store(&hold_completion,0);
        configASSERT(gpu_queue_wait_fence(fence,&record,GPU_QUEUE_WAIT_FOREVER)==GPU_QUEUE_OK && record.sequence==1);
        configASSERT(gpu_queue_submit(context,&job)==GPU_QUEUE_OK &&
            gpu_queue_wait(context,&record,GPU_QUEUE_WAIT_FOREVER)==GPU_QUEUE_OK && record.sequence==2);
        platform_puts("PASS GPU queue case ");platform_hex(c);
        platform_puts("; timeout retains payload and output ownership until the fence is collected\n");platform_exit(0);
    }
    if (c==59) {
        atomic_store(&hold_completion,1);
        for (unsigned i=1;i<=2;++i) {
            struct gpu_queue_job job=fast_job(i,i-1);
            configASSERT(gpu_queue_submit(context,&job)==GPU_QUEUE_OK);
        }
        xTaskNotifyGiveIndexed(compute->peer.task,0);
        configASSERT(ulTaskNotifyTakeIndexed(0,pdTRUE,portMAX_DELAY));
        configASSERT(compute->fast.used==4 && atomic_load(&compute->peer.client_owed)==2);
        atomic_store(&hold_completion,0);
        for (unsigned i=1;i<=2;++i) {
            struct gpu_queue_record record;
            configASSERT(gpu_queue_wait(context,&record,GPU_QUEUE_WAIT_FOREVER)==GPU_QUEUE_OK && record.sequence==i);
        }
        if (!atomic_load(&peer_finished)) configASSERT(ulTaskNotifyTakeIndexed(0,pdTRUE,portMAX_DELAY));
        configASSERT(atomic_load(&peer_finished) && !compute->fast.used && compute->completed_jobs==4 &&
            uxQueueMessagesWaiting(service.requests)==requests);
        platform_puts("PASS GPU queue case ");platform_hex(c);
        platform_puts("; two EL0 clients pipeline independent sequences and direct completion waiters\n");platform_exit(0);
    }
    if (c==55 || c==62) {
        uint64_t payload_before=payload_cache_calls,inputs_before=dirty_input_cache_calls,outputs_before=output_cache_calls;
        configASSERT(word((unsigned char *)compute->firmware[0].storage.cpu+32,4)==(c==55?2:1));
        if (c==62) {
            configASSERT(compute->job_gpu==A733_GPU_QUEUE_JOB_GPU+64 &&
                compute->job_cpu==(unsigned char *)compute->firmware[5].storage.cpu+64 &&
                64+PVR_COMPUTE_FAST_DEPTH*PVR_COMPUTE_FAST_STRIDE<=4096);
            const unsigned char *root=(const unsigned char *)session.table_extent.cpu;
            uint64_t directory=(word(root+(A733_GPU_QUEUE_JOB_GPU>>30)*4,4)&~UINT64_C(15))<<8;
            const unsigned char *dir=root+(directory-session.table_extent.physical);
            uint64_t leaf=word(dir+((A733_GPU_QUEUE_JOB_GPU>>21)&511)*8,8)&~UINT64_C(4095);
            uint64_t pte=word(root+(leaf-session.table_extent.physical)+((A733_GPU_QUEUE_JOB_GPU&0x1fffff)>>12)*8,8);
            configASSERT((pte&~UINT64_C(4095))==compute->firmware[5].pages[0] && (pte&15)==11);
            memset((unsigned char *)compute->firmware[5].storage.cpu+4,0xa5,60);
        }
        for (unsigned batch=0;batch<8;++batch) {
            struct gpu_queue_job jobs[4];
            struct gpu_queue_fence fences[4];
            for (unsigned i=0;i<4;++i) jobs[i]=fast_job(batch*4+i+1,i);
            uint32_t accepted=0;
            configASSERT(gpu_queue_submit_batch(context,jobs,4,fences,&accepted)==GPU_QUEUE_OK && accepted==4);
            struct gpu_queue_job full=fast_job(batch*4+5,0);
            configASSERT(gpu_queue_submit(context,&full)==GPU_QUEUE_BUSY);
            configASSERT(compute->fast.used==4 && atomic_load(&compute->client_owed)==4);
            /* Waiting for the last fence leaves earlier outputs owned. */
            struct gpu_queue_record record;
            configASSERT(gpu_queue_wait_fence(fences[3],&record,GPU_QUEUE_WAIT_FOREVER)==GPU_QUEUE_OK &&
                record.sequence==batch*4+4 && compute->fast.used==3);
            struct gpu_queue_fence forged=fences[0];++forged.sequence;
            memset(&record,0xa5,sizeof(record));
            configASSERT(gpu_queue_wait_fence(forged,&record,0)==GPU_QUEUE_REFUSED &&
                record.sequence==UINT32_C(0xa5a5a5a5) && compute->fast.used==3);
            for (unsigned i=0;i<3;++i) {
                configASSERT(gpu_queue_wait(context,&record,GPU_QUEUE_WAIT_FOREVER)==GPU_QUEUE_OK &&
                    record.sequence==batch*4+i+1 && word((void *)(uintptr_t)(FIXTURE_OUTPUT+i*128),4)==record.sequence);
            }
            configASSERT(!compute->fast.used && uxQueueMessagesWaiting(service.requests)==requests);
            if (c==62) for (unsigned i=4;i<64;++i)
                configASSERT(((unsigned char *)compute->firmware[5].storage.cpu)[i]==0xa5);
        }
        configASSERT(dirty_input_cache_calls-inputs_before==32 && output_cache_calls-outputs_before==32);
        configASSERT(c==62?payload_cache_calls==payload_before:payload_cache_calls>payload_before);
        platform_puts("PASS GPU queue case ");platform_hex(c);
        platform_puts(c==62?"; NC payload PTE, timeline guard and four slots pass with zero payload DC calls\n":
            "; eight four-job batches retain outputs and collect context-bound fences out of order\n");
        platform_exit(0);
    }
    for (unsigned i=1;i<=32;++i) {
        struct gpu_queue_job job=fast_job(i,i%4);
        if (i%3==0) xTaskNotifyGiveIndexed(xTaskGetCurrentTaskHandle(),PVR_COMPUTE_FAST_NOTIFY);
        configASSERT(gpu_queue_submit(context,&job)==GPU_QUEUE_OK && level()==0);
        struct gpu_queue_record record;
        configASSERT(gpu_queue_wait(context,&record,GPU_QUEUE_WAIT_FOREVER)==GPU_QUEUE_OK && record.sequence==i &&
            !record.detail && level()==0 && word((void *)(uintptr_t)(FIXTURE_OUTPUT+(i%4)*128),4)==i);
#ifdef SUNSTONE_GPU_TIMING
        uint64_t counter;
        __asm__ volatile("isb\n mrs %0,cntvct_el0" : "=r"(counter) :: "memory");
        configASSERT(gpu_queue_last_irq_counter(context)>0 && gpu_queue_last_irq_counter(context)<=counter);
#else
        configASSERT(!gpu_queue_last_irq_counter(context));
#endif
        configASSERT(uxQueueMessagesWaiting(service.requests)==requests && !compute->fast.used);
    }
    configASSERT(atomic_load(&fast_consumed)==32 && compute->completed_jobs==32 && irq_count);
    platform_puts("PASS GPU queue case ");platform_hex(c);
    platform_puts("; EL0 caller kicks and collects 32 delayed jobs with worker suspended and ring wrap\n");
    platform_exit(0);
}
static void fast_peer(void)
{
    configASSERT(ulTaskNotifyTakeIndexed(0,pdTRUE,portMAX_DELAY));
    struct gpu_queue_context peer;
    struct gpu_queue_lease_info leases[GPU_QUEUE_LEASES];
    configASSERT(gpu_queue_context_join(&peer,leases)==GPU_QUEUE_OK);
    for (unsigned i=1;i<=2;++i) {
        struct gpu_queue_job job=fast_job(i,i+1);
        configASSERT(gpu_queue_submit(peer,&job)==GPU_QUEUE_OK);
    }
    xTaskNotifyGiveIndexed(service.clients[0].task,0);
    for (unsigned i=1;i<=2;++i) {
        struct gpu_queue_record record;
        configASSERT(gpu_queue_wait(peer,&record,GPU_QUEUE_WAIT_FOREVER)==GPU_QUEUE_OK && record.sequence==i &&
            word((void *)(uintptr_t)(FIXTURE_OUTPUT+(i+1)*128),4)==i);
    }
    atomic_store(&peer_finished,1);
    xTaskNotifyGiveIndexed(service.clients[0].task,0);
    for (;;) ulTaskNotifyTakeIndexed(0,pdTRUE,portMAX_DELAY);
}
