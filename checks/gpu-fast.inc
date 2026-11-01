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
    configASSERT(cdm>=A733_GPU_QUEUE_JOB_GPU+PVR_COMPUTE_QUEUE_CDM);
    uint64_t slot=cdm-A733_GPU_QUEUE_JOB_GPU-PVR_COMPUTE_QUEUE_CDM;
    configASSERT(!(slot%PVR_COMPUTE_FAST_STRIDE) && slot<PVR_COMPUTE_FAST_DEPTH*PVR_COMPUTE_FAST_STRIDE);
    unsigned produced=atomic_load(&fast_produced);
    configASSERT(produced-atomic_load(&fast_consumed)<PVR_COMPUTE_FAST_DEPTH && serial==produced+1);
    struct fast_model_job *j=&fast_jobs[produced%PVR_COMPUTE_FAST_DEPTH];
    *j=(struct fast_model_job){.serial=serial,.cccb=cursor,.kccb=(uint32_t)word(resource(0),4),
        .page=compute->job_cpu+slot,.doorbell=now_us()};
    memcpy(j->snapshot,j->page,sizeof(j->snapshot));
    atomic_store_explicit(&fast_produced,produced+1,memory_order_release);
    return 1;
}
static void fast_model(void *unused)
{
    (void)unused;
    for (;;) {
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
        uint32_t argument=(uint32_t)word(j->page+PVR_COMPUTE_QUEUE_ARGUMENTS,4);
        configASSERT(output>=FIXTURE_OUTPUT && output+4<=FIXTURE_OUTPUT+FIXTURE_OUTPUT_BYTES);
        put((void *)(uintptr_t)output,4,argument);
        __asm__ volatile("dsb sy" ::: "memory");
        set(compute->firmware[5].storage.cpu,j->serial);
        /* Model 2 us from timeline publication to level interrupt assertion. */
        uint64_t published=now_us();
        while (now_us()<published+2) {}
        atomic_store_explicit(&fast_consumed,consumed+1,memory_order_release);
        set(&gpu_status,1);
        *(volatile uint32_t *)(uintptr_t)(GICD+0x200+4*(95/32))=1u<<(95%32);
        __asm__ volatile("dsb sy" ::: "memory");
    }
}
static struct gpu_queue_job fast_job(unsigned sequence,unsigned slot)
{
    struct gpu_queue_job job={.sequence=sequence,.bindings=1,.argument_bytes=4,.groups={1,1,1}};
    job.binding[0]=(struct gpu_queue_binding){.lease=0,.address=slot*128,.sync_offset=slot*128,.sync_bytes=64};
    memcpy(job.arguments,&sequence,4);
    return job;
}
static void fast_cases(unsigned c)
{
    configASSERT(c==54 && level()==0);
    while (service.irq_masked) vTaskDelay(1);
    vTaskSuspend(service.worker);
    unsigned requests=uxQueueMessagesWaiting(service.requests);
    for (unsigned i=1;i<=32;++i) {
        struct gpu_queue_job job=fast_job(i,i%4);
        if (i%3==0) xTaskNotifyGiveIndexed(xTaskGetCurrentTaskHandle(),PVR_COMPUTE_FAST_NOTIFY);
        configASSERT(gpu_queue_submit(context,&job)==GPU_QUEUE_OK && level()==0);
        struct gpu_queue_record record;
        configASSERT(gpu_queue_wait(context,&record,GPU_QUEUE_WAIT_FOREVER)==GPU_QUEUE_OK && record.sequence==i &&
            !record.detail && level()==0 && word((void *)(uintptr_t)(FIXTURE_OUTPUT+(i%4)*128),4)==i);
        configASSERT(uxQueueMessagesWaiting(service.requests)==requests && !compute->fast.used);
    }
    configASSERT(atomic_load(&fast_consumed)==32 && compute->completed_jobs==32 && irq_count);
    platform_puts("PASS GPU queue case ");platform_hex(c);
    platform_puts("; EL0 caller kicks and collects 32 delayed jobs with worker suspended and ring wrap\n");
    platform_exit(0);
}
