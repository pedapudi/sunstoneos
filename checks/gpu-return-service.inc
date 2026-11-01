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
static void return_client(void)
{
    configASSERT(bench_job(0));closed();
    if(gpu_queue_case==32){
        configASSERT(reopen_reply(service.epoch).result==PVR_GPU_SERVICE_OK);
        configASSERT(compute->phase==PVR_COMPUTE_QUEUE_READY);
        configASSERT(bench_job(1));closed();
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
