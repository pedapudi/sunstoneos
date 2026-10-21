/* Actual native owner/allocator/mapping/service. Firmware, GPU writes and
 * isolation observations are explicit model effects, not physical evidence. */
#define client inherited_client
#define kernel_main inherited_kernel_main
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#include "gpu-render.c"
#pragma clang diagnostic pop
#undef client
#undef kernel_main
void kernel_main(void);
static unsigned cleanups,powers,isolated,stopping,event_injected,calls;
static unsigned char before_records[1024];
static void late_event(void)
{
    configASSERT(!event_injected++);
    unsigned write=(unsigned)word(resource(3),4);
    unsigned char *p=resource(4)+write*88;memset(p,0,88);
    put(p,4,PVR_CCB_MAGIC|104);put(p+8,4,1);put(p+12,4,1);
    put(p+16,4,graphics.lists[0].resource.id);put(resource(3),4,(write+1)&31);
}
static int cleanup_irq(void *ctx,uint32_t off,uint32_t *out)
{ callback(ctx);++calls;configASSERT(off==0x8a8);*out=word(resource(3),4)!=word(resource(3)+128,4);return 1; }
static int cleanup_read(void *ctx,uint32_t off,uint32_t width,uint64_t *out)
{
    ++calls;if(!read_reg(ctx,off,width,out))return 0;
    if(stopping && off!=0x100)*out=off==0x3c8?0x7f:off==0x3898?0xff:0;
    return 1;
}
static int cleanup_enable(void *ctx)
{
    ++calls;
    if(gpu_render_case!=2)return enable(ctx);
    callback(ctx);configASSERT(!powered && graphics.phase==A733_GPU_GRAPHICS_READY &&
        compute.compute.phase==PVR_COMPUTE_SERVICE_READY && !service.config.render && !graphics.submission.phase);
    powered=1;return 1;
}
static int cleanup_isolate(void *ctx)
{
    callback(ctx);++calls;configASSERT(stopping && service.queue.phase==PVR_CCB_RETIRED &&
        service.graphics_cleanup_index==service.graphics_cleanup_count && compute.compute.cleanup_status==1);
    isolated=1;running=0;return 1;
}
static int cleanup_kick(void *ctx,uint32_t off,uint32_t value)
{
    ++calls;callback(ctx);configASSERT(off==0xb00 && !value);
    unsigned next=(unsigned)word(resource(0),4),slot=(next-1)&127;
    unsigned char *p=resource(1)+slot*88;uint32_t type=(uint32_t)word(p,4);
    if(type==(PVR_CCB_MAGIC|106)) {
        const uint32_t addresses[]={graphics.render_object.address+152,graphics.render_object.address,
            compute.config.firmware_address[0],graphics.target.data_firmware[1],graphics.target.data_firmware[0],
            graphics.lists[0].firmware_address,graphics.lists[1].firmware_address};
        const uint32_t types[]={0,0,0,1,1,2,2};unsigned index=cleanups+(gpu_render_case==2?2:0);
        configASSERT(index<7 && service.phase==PVR_GPU_SERVICE_GRAPHICS_CLEANUP && service.graphics_events_retired);
        configASSERT(!word(p+4,4) && word(p+8,4)==types[index] && word(p+12,4)==addresses[index]);
        configASSERT(!word(resource(2)+slot*4,4));for(unsigned i=16;i<88;++i)configASSERT(!p[i]);
        if(service.config.render) {
            uintptr_t shared=a733_gpu_graphics_memory_render_shared(&graphics);
            configASSERT(word((void *)(shared+1292),4)==2047 && word((void *)(shared+1356),4)==2047);
        }
        if(index>=3)configASSERT(compute.compute.phase==PVR_COMPUTE_SERVICE_CLEANED);
        ++cleanups;put(resource(0)+128,4,next);
        if(gpu_render_case==4 && index==3)put(resource(2)+((slot+1)&127)*4,4,1);
        else put(resource(2)+slot*4,4,gpu_render_case==3 && index==3?3:1);
        if(gpu_render_case==5 && index==3)late_event();
        return 1;
    }
    if(type==(PVR_CCB_MAGIC|107)) {
        configASSERT(cleanups==(gpu_render_case==2?5u:7u) && service.graphics_events_retired &&
            service.graphics_cleanup_index==service.graphics_cleanup_count && compute.compute.cleanup_status==1);
        ++powers;put(resource(PVR_FW_POWER_SYNC),4,1);
        if(word(p+8,4)==1)stopping=1;
        if(gpu_render_case==6)late_event();
        put(resource(0)+128,4,next);put(resource(2)+slot*4,4,1);return 1;
    }
    if(type==(PVR_CCB_MAGIC|101)) {
        uint32_t *output=(void *)(graphics.payload_extent.cpu+graphics.render.payload_bytes+8192+PVR_RENDER_OUTPUT_OFFSET+64);
        for(unsigned i=0;i<4096;++i)output[i]=PVR_RENDER_PIXEL;
    }
    /* Existing source-exact paired GEOM/PR and final FRAG fixture. Its nominal
     * selector publishes timeline1/1 then final2; restore our case afterward. */
    unsigned selected=gpu_render_case;gpu_render_case=0;int result=kick(ctx,off,value);gpu_render_case=selected;
    return result;
}
static struct pvr_gpu_service_reply wait_terminal(unsigned phase)
{
    struct pvr_gpu_service_reply r;
    for(unsigned i=0;i<1500;++i) {
        r=request(PVR_GPU_SERVICE_STATUS,0);
        if(r.phase==phase)return r;
        configASSERT(r.phase!=PVR_GPU_SERVICE_FAILED);vTaskDelay(1);
    }
    platform_panic("cleanup wait",phase);
}
static void client(void *unused)
{
    (void)unused;configASSERT(request(PVR_GPU_SERVICE_OPEN,3000000).result==PVR_GPU_SERVICE_OK);
    wait_terminal(PVR_GPU_SERVICE_READY);
    if(gpu_render_case!=1 && gpu_render_case!=2) {
        configASSERT(request(PVR_GPU_SERVICE_RENDER,1000000).result==PVR_GPU_SERVICE_OK);
        for(unsigned i=0;i<1500;++i) {
            struct pvr_gpu_service_reply r=request(PVR_GPU_SERVICE_STATUS,0);
            configASSERT(r.phase!=PVR_GPU_SERVICE_FAILED);
            if(r.render_phase==PVR_RENDER_SERVICE_COMPLETE)break;
            vTaskDelay(1);
        }
        configASSERT(graphics.submission.phase==PVR_RENDER_SERVICE_COMPLETE && kicks==1);
        for(unsigned first=0;first<4096;first+=64) {
            uint64_t ticket;struct pvr_gpu_service_reply r;
            configASSERT(pvr_gpu_service_render_read(&service,first,0,&ticket) &&
                pvr_gpu_service_collect(&service,ticket,&r,1000) && r.result==PVR_GPU_SERVICE_OK && r.render_pixel_count==64);
            for(unsigned i=0;i<64;++i)configASSERT(r.render_pixels[i]==PVR_RENDER_PIXEL);
        }
    }
    if(gpu_render_case==7) {
        atomic_uint *states[]={&compute.compute.input_state,&graphics.submission.draw_state,
            &graphics.submission.texture_state,&graphics.submission.texture_output_state};
        for(unsigned i=0;i<4;++i) {
            atomic_store(states[i],1);configASSERT(request(PVR_GPU_SERVICE_CLOSE,1000000).result==PVR_GPU_SERVICE_BUSY);
            atomic_store(states[i],0);configASSERT(!cleanups && !service.graphics_events_retired);
        }
        graphics.submission.texture_draft=1;
        configASSERT(request(PVR_GPU_SERVICE_CLOSE,1000000).result==PVR_GPU_SERVICE_BUSY);
        graphics.submission.texture_draft=0;
    }
    memcpy(before_records,(void *)graphics.shared_extent.cpu,sizeof(before_records));
    configASSERT(request(PVR_GPU_SERVICE_CLOSE,1000000).result==PVR_GPU_SERVICE_OK);
    struct pvr_gpu_service_reply r=wait_terminal(gpu_render_case>=3 && gpu_render_case<=6?PVR_GPU_SERVICE_FAILED:PVR_GPU_SERVICE_STOPPED);
    if(gpu_render_case>=3 && gpu_render_case<=6) {
        configASSERT(r.owns_session && !isolated && compute.compute.cleanup_status==1);
        configASSERT(cleanups==(gpu_render_case==6?7u:4u) && powers==(gpu_render_case==6?1u:0u));
        configASSERT(service.graphics_cleanup_status[0]==1 && service.graphics_cleanup_status[1]==1);
        if(gpu_render_case==3)configASSERT(service.graphics_cleanup_status[2]==3);
        if(gpu_render_case>=5)configASSERT(event_injected==1 && service.free_list_events.phase==PVR_GPU_EVENTS_READY && !service.free_lists.request_count);
        unsigned before=calls;vTaskDelay(5);configASSERT(before==calls);
    } else {
        configASSERT(!r.owns_session && isolated && cleanups==(gpu_render_case==2?5u:7u) && powers==2);
        for(unsigned i=0;i<service.graphics_cleanup_count;++i)configASSERT(service.graphics_cleanup_status[i]==1 &&
            service.graphics_cleanup_tokens[i].epoch==service.epoch && service.graphics_cleanup_tokens[i].serial);
        configASSERT(request(PVR_GPU_SERVICE_OPEN,1000000).result==PVR_GPU_SERVICE_BUSY);
    }
    configASSERT(!memcmp(before_records,(void *)graphics.shared_extent.cpu,sizeof(before_records)));
    held(firmware.wb_pool,graphics.backing_lease);held(firmware.wb_pool,graphics.payload_lease);
    held(firmware.nc_pool,graphics.shared_lease);held(firmware.wb_pool,graphics.table_lease);
    platform_puts("PASS render cleanup case ");platform_hex(gpu_render_case);platform_puts("; exact receipts and retained allocations\n");platform_exit(0);
}
void kernel_main(void)
{
    platform_init();configASSERT(gpu_render_case<8);
    configASSERT(a733_memory_reservations_init() && a733_memory_uncached_init());
    configASSERT(a733_gpu_reserved_prepare(&firmware,9,gpu_firmware,(size_t)(gpu_firmware_end-gpu_firmware),400000000));
    configASSERT(a733_gpu_compute_reserved_prepare(&compute,&firmware,&service,10));
    configASSERT((gpu_render_case==2?a733_gpu_graphics_memory_prepare:a733_gpu_graphics_memory_prepare_clear)
        (&graphics,&firmware,&compute,&service,11,&profile));
    configASSERT(a733_gpu_graphics_memory_validate(&graphics,&firmware.config,&service));
    struct pvr_gpu_service_config *c=&firmware.config;
    c->queue=(struct pvr_ccb_ops){read_queue,write_queue,barrier,cleanup_kick};
    c->start=(struct pvr_gpu_start_ops){now,cleanup_read,write_reg,started,publish};
    c->control=(struct pvr_gpu_control_ops){now,cleanup_irq,irq_write,compat,NULL};
    c->stop=(struct pvr_gpu_stop_ops){now,cleanup_read,write_reg,sync_write,sync_read,cleanup_isolate,NULL};
    c->enable=cleanup_enable;c->compute_mode=mode;
    configASSERT(pvr_gpu_service_init(&service,c,3,1));
    TaskHandle_t t=xTaskCreateStaticAffinitySet(client,"cleanup-check",4096,NULL,2,client_stack,&client_state,2);
    configASSERT(t && pvr_gpu_service_bind(&service,0,t));vTaskStartScheduler();platform_panic("scheduler",0);
}
