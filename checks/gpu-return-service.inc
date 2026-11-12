/* Real FreeRTOS service and native firmware memory; supply-end is a modeled
 * callback boundary, covered with the actual provider in gpu-return-supply.c. */
static unsigned park_calls;
static uint64_t park_deadline;
static int final_park(void *ctx,uint64_t deadline)
{
    callback(ctx);
    configASSERT(service.phase==PVR_GPU_SERVICE_PARKING &&
                 __wrap_a733_gpu_backend_stopped(NULL,&service));
    if(park_calls++)configASSERT(deadline==park_deadline);
    park_deadline=deadline;
    return gpu_queue_case==33 ? -1 : park_calls==2;
}
static void failure_return_client(unsigned c)
{
    unsigned submitted=c==70||c==73;
    if(submitted){
        struct gpu_queue_job job=c==73?fast_job(1,0):make_job(0);
        if(c==73)atomic_store(&hold_completion,1);
        configASSERT(gpu_queue_submit(context,&job)==GPU_QUEUE_OK);
        if(c==70){
            TickType_t start=xTaskGetTickCount();
            while(compute->phase!=PVR_COMPUTE_QUEUE_READY || !uxQueueMessagesWaiting(compute->completions)){
                configASSERT(xTaskGetTickCount()-start<1000);vTaskDelay(1);
            }
        }
    }
    if(c==71)atomic_store(&compute->slot_state,PVR_COMPUTE_QUEUE_RESERVED);
    if(c==74)atomic_store(&compute->lost,1);
    if(c==75)atomic_store(&compute->fast.locked,1);
    uint64_t end=now_us()+((c==71||c==75)?10000:3000000);
    configASSERT(pvr_gpu_service_quiesce(&service,end)==1);
    unsigned again=pvr_gpu_service_quiesce(&service,end+1000000);
    configASSERT((again==1||again==2||(c==74&&again==3)) && service.quiesce.deadline==end);
    if(c==73){
        vTaskDelay(1);
        configASSERT(atomic_load(&service.quiesce.state)==1 && !park_calls && compute->fast.used==1);
        atomic_store(&hold_completion,0);
    }
    while(atomic_load_explicit(&service.quiesce.state,memory_order_acquire)==1){
        configASSERT(now_us()<end+10000);vTaskDelay(1);
    }
    if(c==75){
        configASSERT(atomic_load(&service.quiesce.state)==3 && !park_calls &&
            atomic_load(&compute->fast.locked) && !atomic_load(&compute->lost));
    }else if(c==71||c==74){
        configASSERT(atomic_load(&service.quiesce.state)==3 && !park_calls &&
            service.phase==PVR_GPU_SERVICE_FAILED && atomic_load(&compute->lost));
        if(c==71)configASSERT(atomic_load(&compute->slot_state)==PVR_COMPUTE_QUEUE_RESERVED);
    }else if(c==72){
        configASSERT(atomic_load(&service.quiesce.state)==2 && service.phase==PVR_GPU_SERVICE_COLD && !enabled && !park_calls);
    }else{
        configASSERT(atomic_load(&service.quiesce.state)==2 && service.phase==PVR_GPU_SERVICE_PARKED &&
            park_calls==2 && isolated && pvr_compute_queue_clients_idle(compute) &&
            service.quiesce.abandoned==submitted && compute->completed_jobs==submitted);
    }
    platform_puts("PASS GPU queue case ");platform_hex(c);
    platform_puts("; worker failure return preserves unfinished ownership and drains completed outputs\n");platform_exit(0);
}
static void return_client(void)
{
    configASSERT(grid_job(0));closed();
    if(gpu_queue_case==32){
        configASSERT(reopen_reply(service.epoch).result==PVR_GPU_SERVICE_OK);
        configASSERT(compute->phase==PVR_COMPUTE_QUEUE_READY);
        configASSERT(grid_job(1));closed();
    }
    struct pvr_gpu_service_request request={PVR_GPU_SERVICE_PARK,1000000};
    struct pvr_gpu_service_reply reply;uint64_t ticket;
    configASSERT(pvr_gpu_service_send(&service,&request,10,&ticket));
    while(!pvr_gpu_service_collect(&service,ticket,&reply,1)){}
    configASSERT(reply.result==PVR_GPU_SERVICE_OK);
    while(service.phase==PVR_GPU_SERVICE_PARKING)vTaskDelay(1);
    if(gpu_queue_case==33){
        configASSERT(service.phase==PVR_GPU_SERVICE_FAILED && service.error==PVR_GPU_SERVICE_STOP && park_calls==1);
    }else{
        configASSERT(service.phase==PVR_GPU_SERVICE_PARKED && park_calls==2 &&
                     __wrap_a733_gpu_backend_stopped(NULL,&service));
        const enum pvr_gpu_service_operation refused[]={PVR_GPU_SERVICE_OPEN,PVR_GPU_SERVICE_PARK,PVR_GPU_SERVICE_DISPOSE};
        for(unsigned i=0;i<3;++i){
            request.operation=refused[i];
            configASSERT(pvr_gpu_service_send(&service,&request,10,&ticket));
            while(!pvr_gpu_service_collect(&service,ticket,&reply,1)){}
            configASSERT(reply.result!=PVR_GPU_SERVICE_OK && park_calls==2);
        }
        configASSERT(!service.clients[0].pending && !uxQueueMessagesWaiting(service.requests) &&
                     !uxQueueMessagesWaiting(compute->completions));
    }
    vTaskSuspend(service.worker);
    platform_puts("PASS GPU queue case ");platform_hex(gpu_queue_case);
    platform_puts("; final PARK after acknowledged GPU OFF, thermal reopen and retained callback fault\n");platform_exit(0);
}
