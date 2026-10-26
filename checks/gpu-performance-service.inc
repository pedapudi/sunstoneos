/* Selected additions to the actual FreeRTOS Mimus service fixture. No model
 * inference: one admitted synthetic job checks slot reuse after RELEASE. */
static StaticTask_t perf_task;
static StackType_t perf_stack[4096];
static atomic_uint perf_go, perf_done, perf_held, perf_observed;
static unsigned perf_rate=1008000000, perf_mv=960, perf_action, perf_value, perf_commands;
static uint64_t perf_end, perf_limit, perf_elapsed[6];
static int perf_kick(void *ctx,unsigned next,unsigned slot,unsigned char *cmd)
{
    const unsigned type=(unsigned)word(cmd,4),request=(unsigned)word(cmd+12,4);
    if(type!=(PVR_CCB_MAGIC|204u) && !(type==(PVR_CCB_MAGIC|107u) &&
        service.performance.clock.phase!=PVR_GPU_CLOCK_NEW && service.performance.clock.phase!=PVR_GPU_CLOCK_READY))return 0;
    callback(ctx);++perf_commands;
    if(type==(PVR_CCB_MAGIC|204u)) {
        configASSERT(word(cmd+8,4)==perf_rate && word(resource(PVR_FW_RUNTIME_CONFIG)+12,4)==perf_rate);
        configASSERT(word(resource(PVR_FW_UTILIZATION)+10240,4)==1);
    } else {
        configASSERT(word(cmd+8,4)==2 && (request==1 || request==2) && !word(resource(PVR_FW_POWER_SYNC),4));
        if(!((mimus_service_case==27 && request==1) || (mimus_service_case==28 && request==2)))
            put(resource(PVR_FW_POWER_SYNC),4,1);
        put(resource(PVR_FW_SYSTEM_DATA)+8,4,request==1?2:3);
    }
    put(resource(0)+128,4,next);put(resource(2)+slot*4,4,1);return 1;
}
static int perf_state(void *ctx,uint32_t *value)
{callback(ctx);return a733_gpu_memory_power_state(&memory,value);}
static int perf_publish(void *ctx,uint32_t hz)
{
    callback(ctx);configASSERT(hz==perf_rate);
    uint64_t ns=now_us()*1000;
    configASSERT(a733_gpu_memory_clock(&memory,hz,ns,123456));
    const unsigned char *entry=resource(PVR_FW_UTILIZATION)+40;
    configASSERT(word(entry,8)==ns && word(entry+8,8)==ns && word(entry+16,8)==123456 &&
        word(entry+24,8)==(UINT64_C(256000000)<<20)/(hz/1000) && word(entry+32,4)==hz && !word(entry+36,4));
    return 1;
}
static int perf_begin(void *ctx,enum a733_perf_action action,uint32_t value,uint64_t deadline)
{
    callback(ctx);configASSERT(now_us()<deadline && (action==A733_PERF_READ || service.performance.clock.phase==PVR_GPU_CLOCK_HELD));
    perf_action=action;perf_value=value;perf_end=deadline;return 1;
}
static int perf_poll(void *ctx,struct a733_perf_point *point)
{
    callback(ctx);configASSERT(now_us()<perf_end);
    if(perf_action==A733_PERF_RATE) {configASSERT(perf_value==600 || perf_mv==960);perf_rate=perf_value*1000000;}
    if(perf_action==A733_PERF_VOLTAGE){configASSERT(perf_value==960 || perf_rate==600000000);perf_mv=perf_value;}
    *point=(struct a733_perf_point){perf_rate/1000000,perf_mv};return 1;
}
static uint32_t perf_current(void *ctx){callback(ctx);return perf_rate;}
static enum a733_perf_status perf_request(enum a733_perf_action action,unsigned value)
{
    uint64_t started=now_us();
    uint64_t ticket,end=started+(mimus_service_case==29 && action==A733_PERF_HOLD?10000:50000);
    if(action==A733_PERF_HOLD)perf_limit=end+150000;
    if(!perf_limit)perf_limit=end;
    configASSERT(pvr_gpu_service_performance(&service,action,value,end,perf_limit,&ticket));
    struct pvr_gpu_service_reply reply;
    while(!pvr_gpu_service_collect(&service,ticket,&reply,1)) {
        configASSERT(now_us()<end+20000);
        if(mimus_service_case==26 && action==A733_PERF_HOLD && atomic_load(&hold_completion) && end-now_us()<40000) {
            configASSERT(!perf_commands && mimus->phase==PVR_MIMUS_SERVICE_IN_FLIGHT);
            atomic_store(&hold_completion,0);
        }
    }
    perf_elapsed[action]=now_us()-started;
    return reply.performance_status;
}
static void performance_owner(void *arg)
{
    (void)arg;while(!atomic_load(&perf_go))vTaskDelay(1);
    enum a733_perf_status held=perf_request(A733_PERF_HOLD,0);
    if(mimus_service_case==27)configASSERT(held==A733_PERF_FAILED);
    else if(mimus_service_case==29) {
        configASSERT(held==A733_PERF_EXPIRED && !perf_commands && perf_request(A733_PERF_RELEASE,0)==A733_PERF_DONE);
    } else {
        configASSERT(held==A733_PERF_DONE);atomic_store(&perf_held,1);
        while(!atomic_load(&perf_observed))vTaskDelay(1);
        configASSERT(perf_request(A733_PERF_RATE,600)==A733_PERF_DONE);
        configASSERT(perf_request(A733_PERF_VOLTAGE,800)==A733_PERF_DONE);
        atomic_store(&perf_held,0);
        configASSERT(perf_request(A733_PERF_RELEASE,0)==(mimus_service_case==28?A733_PERF_FAILED:A733_PERF_DONE));
        if(mimus_service_case!=28)configASSERT(perf_request(A733_PERF_READ,0)==A733_PERF_DONE);
    }
    atomic_store(&perf_done,1);vTaskSuspend(NULL);
}
static void performance_client(void)
{
    const struct pvr_mimus_request job={0,fixture_jobs[0].ordinal,fixture_jobs[0].rows,2*fixture_jobs[0].k};
    configASSERT(pvr_gpu_service_performance_state(&service)==(PVR_GPU_PERF_OWNER|PVR_GPU_PERF_READY));
    if(mimus_service_case==29)atomic_store(&mimus->slot_state,PVR_MIMUS_SLOT_RESERVED);
    if(mimus_service_case==26) {
        atomic_store(&hold_completion,1);
        configASSERT(pvr_mimus_service_submit(mimus,&job)==1);
        while(mimus->phase!=PVR_MIMUS_SERVICE_IN_FLIGHT)vTaskDelay(1);
    }
    atomic_store(&perf_go,1);
    if(mimus_service_case==26) {
        struct pvr_mimus_record record=receive();
        configASSERT(record.result==PVR_MIMUS_OK && record.sequence==1 && sums_equal(0,1));
        completed=1;
    }
    while(!atomic_load(&perf_done)) {
        if(atomic_load(&perf_held)) {
            configASSERT(pvr_mimus_service_submit(mimus,&job)==PVR_MIMUS_SUBMIT_HELD);
            configASSERT(close_reply().result==PVR_GPU_SERVICE_BUSY);
            atomic_store(&perf_observed,1);
        }
        vTaskDelay(1);
    }
    if(mimus_service_case==27 || mimus_service_case==28) {
        configASSERT(service.phase==PVR_GPU_SERVICE_FAILED && atomic_load(&mimus->slot_state)!=PVR_MIMUS_SLOT_IDLE);
        configASSERT(pvr_gpu_service_performance_state(&service)==(PVR_GPU_PERF_OWNER|PVR_GPU_PERF_FAILED));
        configASSERT(close_reply().result==PVR_GPU_SERVICE_FAULT);
    } else {
        if(mimus_service_case==29)atomic_store(&mimus->slot_state,PVR_MIMUS_SLOT_IDLE);
        configASSERT(bench_job(mimus_service_case==26?1:0));closed();
        configASSERT(pvr_gpu_service_performance_state(&service)==PVR_GPU_PERF_OWNER);
        configASSERT(reopen_reply(service.epoch).result==PVR_GPU_SERVICE_OK);
        struct pvr_mimus_record record=receive();configASSERT(record.result==PVR_MIMUS_OK && record.sequence==0 && mimus->sequence==(mimus_service_case==26?2u:1u));
        configASSERT(pvr_gpu_service_performance_state(&service)==(PVR_GPU_PERF_OWNER|PVR_GPU_PERF_READY));
    }
    vTaskSuspend(service.worker);
    size_t untouched=0;
    while(untouched<sizeof(service.worker_stack) && ((const unsigned char *)service.worker_stack)[untouched]==0xa5)++untouched;
    metric("worker-stack-bytes",sizeof(service.worker_stack));metric("worker-stack-untouched",untouched);
    metric("service-bytes",sizeof(service));metric("request-queue-bytes",sizeof(service.request_storage));
    metric("performance-hold-us",perf_elapsed[A733_PERF_HOLD]);metric("performance-rate-us",perf_elapsed[A733_PERF_RATE]);
    metric("performance-voltage-us",perf_elapsed[A733_PERF_VOLTAGE]);metric("performance-release-us",perf_elapsed[A733_PERF_RELEASE]);
    metric("performance-read-us",perf_elapsed[A733_PERF_READ]);
    platform_puts("PASS mimus service case ");platform_hex(mimus_service_case);
    platform_puts("; GPU performance mailbox, slot ownership, native memory publication and retained lifecycle\n");platform_exit(0);
}
