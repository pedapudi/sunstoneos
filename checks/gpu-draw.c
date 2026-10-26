/* Reuse the accepted actual native graphics owner and firmware/ring model.
 * GPU writes and firmware register replies below are explicit model effects. */
#define client fixed_render_client
#define kernel_main fixed_render_kernel_main
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#include "gpu-render.c"
#pragma clang diagnostic pop
#undef client
#undef kernel_main
void kernel_main(void);
static unsigned stopping,isolated;
static uint32_t source[126*8];
static uint32_t expected_vertices[126*8];
static unsigned vertex_count;
static uintptr_t draw_input(void)
{ return graphics.payload_extent.cpu+graphics.render.payload_bytes+PVR_RENDER_PROGRAM_BYTES+4096u; }
static int draw_read_reg(void *ctx,uint32_t off,uint32_t width,uint64_t *out)
{
    if(!read_reg(ctx,off,width,out))return 0;
    if(stopping && off!=0x100)*out=off==0x3c8?0x7f:off==0x3898?0xff:0;
    return 1;
}
static int isolate(void *ctx)
{
    callback(ctx);configASSERT(stopping && service.queue.phase==PVR_CCB_RETIRED);
    /* The vendor RGXStop asserts no reset: SOFT_RESET keeps the start's release value. */
    configASSERT(!word(registers+0x100,8));
    running=0;isolated=1;return 1;
}
static int draw_kick(void *ctx,uint32_t off,uint32_t value)
{
    unsigned next=(unsigned)word(resource(0),4),slot=(next-1)&127;
    unsigned char *command=resource(1)+slot*88;
    uint32_t type=(uint32_t)word(command,4);
    if(type==(PVR_CCB_MAGIC|107)) {
        callback(ctx);configASSERT(running && off==0xb00 && !value);
        configASSERT(!word(resource(PVR_FW_POWER_SYNC),4));put(resource(PVR_FW_POWER_SYNC),4,1);
        if(word(command+8,4)==1)stopping=1;
        put(resource(0)+128,4,next);put(resource(2)+slot*4,4,1);return 1;
    }
    if(type==(PVR_CCB_MAGIC|117)) {
        configASSERT(graphics.draw==1 && vertex_count>=3 && !memcmp((void *)draw_input(),expected_vertices,vertex_count*32));
        uintptr_t payload=graphics.payload_extent.cpu+graphics.render.payload_bytes;
        configASSERT(word((void *)(payload+8192+PVR_RENDER_DRAW_COUNT_OFFSET),4)==vertex_count);
        configASSERT(word(graphics.submission.image.fragment+112,8)==PVR_RENDER_DRAW_SCISSOR);
        uint32_t *pixels=(void *)(payload+8192+PVR_RENDER_OUTPUT_OFFSET+64);
        /* Only this explicit GPU model boundary writes device output. */
        for(unsigned i=0;i<4096;++i)pixels[i]=0xff000000u|i;
        if(gpu_render_case==1)pixels[-1]^=1;
    }
    int result=kick(ctx,off,value);
    if(type==(PVR_CCB_MAGIC|117) && gpu_render_case==1) {
        uintptr_t timelines=a733_gpu_graphics_memory_render_shared(&graphics)+1408;
        __atomic_store_n((uint32_t *)timelines,1,__ATOMIC_RELEASE);
        __atomic_store_n((uint32_t *)(timelines+4),1,__ATOMIC_RELEASE);
    }
    return result;
}
static void make_vertices(unsigned count)
{
    memset(source,0,sizeof(source));vertex_count=count;
    for(unsigned i=0;i<count;++i) {
        source[i*8+3]=0x3f800000;source[i*8+4]=0x3f800000;source[i*8+7]=0x3f800000;
    }
}
static void draw_client(void *unused)
{
    (void)unused;struct pvr_gpu_service_reply r=request(PVR_GPU_SERVICE_OPEN,5000000);
    configASSERT(r.result==PVR_GPU_SERVICE_OK);
    for(unsigned i=0;i<4000;++i){vTaskDelay(1);r=request(PVR_GPU_SERVICE_STATUS,0);configASSERT(r.result!=PVR_GPU_SERVICE_FAULT);if(r.phase==PVR_GPU_SERVICE_READY)break;}
    configASSERT(r.phase==PVR_GPU_SERVICE_READY && r.epoch==1);
    uint64_t ticket;make_vertices(126);
    memcpy(expected_vertices,source,sizeof(source));
    configASSERT(!pvr_gpu_service_draw(&service,1,source,sizeof(source)-4,126,1000000,0,&ticket));
    source[0]=0x7fc00000;configASSERT(!pvr_gpu_service_draw(&service,1,source,sizeof(source),126,1000000,0,&ticket));source[0]=0;
    configASSERT(pvr_gpu_service_draw(&service,2,source,sizeof(source),126,1000000,0,&ticket));
    configASSERT(pvr_gpu_service_collect(&service,ticket,&r,1000) && r.result==PVR_GPU_SERVICE_INVALID);
    configASSERT(!atomic_load(&graphics.submission.draw_state) && !word((void *)draw_input(),4));
    /* Revoke ownership only after queue copy; actual worker consumption must
     * refuse it and free the scoped staging slot exactly once. */
    vTaskSuspend(service.worker);
    configASSERT(pvr_gpu_service_draw(&service,1,source,sizeof(source),126,1000000,0,&ticket));
    service.owner=PVR_GPU_SERVICE_CLIENTS;vTaskResume(service.worker);
    configASSERT(pvr_gpu_service_collect(&service,ticket,&r,1000) && r.result==PVR_GPU_SERVICE_DENIED);
    configASSERT(!atomic_load(&graphics.submission.draw_state));service.owner=0;
    vTaskSuspend(service.worker);
    struct pvr_gpu_service_message filler={0};
    for(unsigned i=0;i<PVR_GPU_SERVICE_CLIENTS;++i)configASSERT(xQueueSend(service.requests,&filler,0)==pdTRUE);
    configASSERT(!pvr_gpu_service_draw(&service,1,source,sizeof(source),126,1000000,0,&ticket));
    configASSERT(!atomic_load(&graphics.submission.draw_state) && !service.clients[0].pending);
    configASSERT(xQueueReset(service.requests)==pdPASS);vTaskResume(service.worker);
    /* Real CPU translation and cache helpers publish the private input/VDM
     * pages; source changes after enqueue cannot alter accepted bytes. */
    vTaskSuspend(service.worker);
    configASSERT(pvr_gpu_service_draw(&service,1,source,sizeof(source),126,1000000,0,&ticket));
    memset(source,0xa5,sizeof(source));
    configASSERT(!pvr_gpu_service_draw(&service,1,source,sizeof(source),126,1000000,0,&ticket));
    vTaskResume(service.worker);
    configASSERT(pvr_gpu_service_collect(&service,ticket,&r,1000) && r.result==PVR_GPU_SERVICE_OK);
    configASSERT(!atomic_load(&graphics.submission.draw_state));
    for(unsigned i=0;i<2000;++i){vTaskDelay(1);r=request(PVR_GPU_SERVICE_STATUS,0);if(r.phase==PVR_GPU_SERVICE_FAILED || r.render_phase==PVR_RENDER_SERVICE_COMPLETE)break;}
    configASSERT(kicks==1);
    if(gpu_render_case) {
        configASSERT(r.phase==PVR_GPU_SERVICE_FAILED && r.error==PVR_GPU_SERVICE_GRAPHICS);
        configASSERT(graphics.submission.error==(gpu_render_case==1?PVR_RENDER_SERVICE_RESULT:PVR_RENDER_SERVICE_QUEUE));
        held(firmware.wb_pool,graphics.payload_lease);
        platform_puts("PASS draw: guarded output or uncertain paired submission retains native owners\n");platform_exit(0);
    }
    configASSERT(r.render_phase==PVR_RENDER_SERVICE_COMPLETE && r.render_checked==PVR_RENDER_OUTPUT_BYTES);
    for(unsigned first=0;first<4096;first+=64) {
        configASSERT(pvr_gpu_service_render_read(&service,first,0,&ticket));
        configASSERT(pvr_gpu_service_collect(&service,ticket,&r,1000) && r.result==PVR_GPU_SERVICE_OK && r.render_pixel_count==64);
        for(unsigned i=0;i<64;++i)configASSERT(r.render_pixels[i]==(0xff000000u|first+i));
    }
    make_vertices(126);
    configASSERT(pvr_gpu_service_draw(&service,1,source,sizeof(source),126,1000000,0,&ticket));
    configASSERT(pvr_gpu_service_collect(&service,ticket,&r,1000) && r.result==PVR_GPU_SERVICE_BUSY);
    configASSERT(!atomic_load(&graphics.submission.draw_state));
    r=request(PVR_GPU_SERVICE_CLOSE,1000000);configASSERT(r.result==PVR_GPU_SERVICE_OK);
    for(unsigned i=0;i<2000;++i){vTaskDelay(1);r=request(PVR_GPU_SERVICE_STATUS,0);if(r.phase==PVR_GPU_SERVICE_STOPPED)break;configASSERT(r.phase!=PVR_GPU_SERVICE_FAILED);}
    configASSERT(r.phase==PVR_GPU_SERVICE_STOPPED && isolated && !running);
    platform_puts("PASS draw: actual native owner/mappings, copied126-vertex admission, paired timelines, arbitrary copied framebuffer and healthy CLOSE\n");platform_exit(0);
}
void kernel_main(void)
{
    platform_init();configASSERT(gpu_render_case<3);
    configASSERT(a733_memory_reservations_init() && a733_memory_uncached_init());
    configASSERT(a733_gpu_reserved_prepare(&firmware,9,gpu_firmware,(size_t)(gpu_firmware_end-gpu_firmware),400000000));
    configASSERT(a733_gpu_compute_reserved_prepare(&compute,&firmware,&service,10));
    configASSERT(a733_gpu_graphics_memory_prepare_draw(&graphics,&firmware,&compute,&service,11,&profile));
    configASSERT(a733_gpu_graphics_memory_validate(&graphics,&firmware.config,&service));
    struct pvr_gpu_service_config *c=&firmware.config;
    c->queue=(struct pvr_ccb_ops){read_queue,write_queue,barrier,draw_kick};
    c->start=(struct pvr_gpu_start_ops){now,read_reg,write_reg,started,publish};
    c->control=(struct pvr_gpu_control_ops){now,irq_read,irq_write,compat,NULL};
    c->stop=(struct pvr_gpu_stop_ops){now,draw_read_reg,write_reg,sync_write,sync_read,isolate,NULL};
    c->enable=enable;c->compute_mode=mode;
    configASSERT(pvr_gpu_service_init(&service,c,3,1));
    TaskHandle_t t=xTaskCreateStaticAffinitySet(draw_client,"draw-check",4096,NULL,2,client_stack,&client_state,2);
    configASSERT(t && pvr_gpu_service_bind(&service,0,t));vTaskStartScheduler();platform_panic("scheduler",0);
}
