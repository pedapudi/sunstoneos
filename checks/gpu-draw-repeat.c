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
    configASSERT(word(registers+0x100,8)==UINT64_C(0xffeffffffffffc3d));
    running=0;isolated=1;return 1;
}
static unsigned geometry_wrapped,fragment_wrapped;
static void stream_commands(uintptr_t base,struct pvr_cccb *ring,
    const struct pvr_cccb_command *expected,unsigned count,unsigned *wrapped)
{
    unsigned cursor=ring->write;
    for(unsigned i=0;i<count;++i) {
        unsigned char *p=(void *)(base+cursor);
        if(word(p,4)==(PVR_CCB_MAGIC|221)) {
            configASSERT(cursor+40+word(p+4,4)==2048);cursor=0;p=(void *)base;*wrapped=1;
        }
        configASSERT(word(p,4)==expected[i].type && word(p+4,4)==expected[i].bytes);
        configASSERT(!memcmp(p+40,expected[i].payload,expected[i].bytes));cursor+=40+expected[i].bytes;
    }
    configASSERT(cursor==ring->cursor);
}
static int draw_kick(void *ctx,uint32_t off,uint32_t value)
{
    callback(ctx);configASSERT(running && off==0xb00 && !value);
    unsigned next=(unsigned)word(resource(0),4),slot=(next-1)&127;
    unsigned char *command=resource(1)+slot*88;
    uint32_t type=(uint32_t)word(command,4);
    if(type==(PVR_CCB_MAGIC|107)) {
        configASSERT(!word(resource(PVR_FW_POWER_SYNC),4));put(resource(PVR_FW_POWER_SYNC),4,1);
        if(word(command+8,4)==1)stopping=1;
    } else if(type==(PVR_CCB_MAGIC|117)) {
        ++kicks;uint32_t frame=graphics.submission.sequence;
        configASSERT(frame==kicks && !memcmp((void *)draw_input(),expected_vertices,vertex_count*32));
        uintptr_t shared=a733_gpu_graphics_memory_render_shared(&graphics);
        const struct pvr_render_job_image *im=&graphics.submission.image;
        struct pvr_cccb_command geom[]={
            {PVR_CCB_MAGIC|PVR_CCCB_TASK|201,112,frame,frame,im->geometry},
            {PVR_CCB_MAGIC|213,8,0,0,im->geometry_update}};
        struct pvr_cccb_command frag[]={
            {PVR_CCB_MAGIC|215,8,0,0,im->fragment_fence},
            {PVR_CCB_MAGIC|PVR_CCCB_TASK|203,480,frame,frame,im->fragment},
            {PVR_CCB_MAGIC|213,8,0,0,im->fragment_update}};
        stream_commands(shared+4096,&graphics.submission.geometry,geom,2,&geometry_wrapped);
        stream_commands(shared+8192,&graphics.submission.fragment,frag,3,&fragment_wrapped);
        unsigned rt=(frame-1)&1;uint32_t hwrt=graphics.target.data_firmware[rt];
        configASSERT(word(im->geometry+4,4)==hwrt && word(im->fragment+4,4)==hwrt);
        configASSERT(word(command+24,4)==hwrt+320 && word(command+56,4)==hwrt+320);
        configASSERT(word(im->geometry+88,4)==frame-1 && word(im->geometry_update+4,4)==frame &&
            word(im->fragment_fence+4,4)==frame && word(im->fragment_update+4,4)==frame);
        configASSERT(word(im->fragment+56,4)==0x300 && word(im->fragment+68,4)==0x80030000);
        uintptr_t payload=graphics.payload_extent.cpu+graphics.render.payload_bytes;
        configASSERT(word((void *)(payload+8192+PVR_RENDER_DRAW_COUNT_OFFSET),4)==vertex_count);
        uint32_t *pixels=(void *)(payload+8192+PVR_RENDER_OUTPUT_OFFSET+64);
        for(unsigned i=0;i<4096;++i)configASSERT(pixels[i]==PVR_RENDER_POISON);
        /* Model only GPU/firmware effects. Native code must preserve RT state
         * and advance both existing rings without formatting them again. */
        unsigned char *record=graphics.target.data[rt];
        configASSERT(word(record+320,4)==(frame-1)/2*2 && word(record+324,4)==word(record+320,4));
        if(frame>2)configASSERT(word(record+168,4)==4 && word(record+172,4)==5);
        put(record+320,4,word(record+320,4)+2);put(record+324,4,word(record+324,4)+2);
        put(record+168,4,4);put(record+172,4,5);
        for(unsigned i=0;i<4096;++i)pixels[i]=0xff000000u|(frame<<12)|i;
        if(frame==2 && gpu_render_case==1)pixels[-1]^=1;
        if(frame==2 && gpu_render_case==2)return 0;
        __atomic_store_n((uint32_t *)(shared+1280+4),graphics.submission.geometry.cursor,__ATOMIC_RELEASE);
        __atomic_store_n((uint32_t *)(shared+1344+4),graphics.submission.fragment.cursor,__ATOMIC_RELEASE);
        __atomic_store_n((uint32_t *)(shared+1408),frame,__ATOMIC_RELEASE);
        __atomic_store_n((uint32_t *)(shared+1412),frame,__ATOMIC_RELEASE);
    } else configASSERT(type==PVR_CCB_HEALTH);
    put(resource(0)+128,4,next);put(resource(2)+slot*4,4,1);return 1;
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
    for(unsigned frame=1;frame<=12;++frame) {
        make_vertices(frame&1?126:3);
        source[4]=frame&1?0x3f800000:0;source[5]=frame&1?0:0x3f800000;
        memcpy(expected_vertices,source,sizeof(source));
        unsigned bytes=vertex_count*32;
        if(frame>1) {
            configASSERT(pvr_gpu_service_draw_next(&service,1,frame-2,source,bytes,vertex_count,1000000,0,&ticket));
            configASSERT(pvr_gpu_service_collect(&service,ticket,&r,1000) && r.result==PVR_GPU_SERVICE_INVALID && r.render_sequence==frame-1);
            configASSERT(!atomic_load(&graphics.submission.draw_state));
            configASSERT(pvr_gpu_service_render_read_sequence(&service,frame-1,0,0,&ticket));
            configASSERT(pvr_gpu_service_collect(&service,ticket,&r,1000) && r.result==PVR_GPU_SERVICE_OK && r.render_sequence==frame-1);
        }
        vTaskSuspend(service.worker);
        configASSERT(pvr_gpu_service_draw_next(&service,1,frame-1,source,bytes,vertex_count,1000000,0,&ticket));
        memset(source,0xa5,sizeof(source));vTaskResume(service.worker);
        configASSERT(pvr_gpu_service_collect(&service,ticket,&r,1000) && r.result==PVR_GPU_SERVICE_OK && r.render_sequence==frame);
        configASSERT(!atomic_load(&graphics.submission.draw_state));
        for(unsigned i=0;i<2000;++i){vTaskDelay(1);r=request(PVR_GPU_SERVICE_STATUS,0);if(r.phase==PVR_GPU_SERVICE_FAILED || r.render_phase==PVR_RENDER_SERVICE_COMPLETE)break;}
        if(frame==2 && gpu_render_case) {
            configASSERT(kicks==2 && r.phase==PVR_GPU_SERVICE_FAILED && r.error==PVR_GPU_SERVICE_GRAPHICS);
            configASSERT(graphics.submission.error==(gpu_render_case==1?PVR_RENDER_SERVICE_RESULT:PVR_RENDER_SERVICE_QUEUE));
            held(firmware.wb_pool,graphics.payload_lease);
            platform_puts("PASS repeated draw: second-frame guard fault or uncertain kick retains native owners\n");platform_exit(0);
        }
        configASSERT(kicks==frame && r.render_phase==PVR_RENDER_SERVICE_COMPLETE && r.render_checked==PVR_RENDER_OUTPUT_BYTES);
        if(frame>1) {
            configASSERT(pvr_gpu_service_render_read_sequence(&service,frame-1,0,0,&ticket));
            configASSERT(pvr_gpu_service_collect(&service,ticket,&r,1000) && r.result==PVR_GPU_SERVICE_INVALID && !r.render_pixel_count);
        }
        for(unsigned first=0;first<4096;first+=64) {
            configASSERT(pvr_gpu_service_render_read_sequence(&service,frame,first,0,&ticket));
            configASSERT(pvr_gpu_service_collect(&service,ticket,&r,1000) && r.result==PVR_GPU_SERVICE_OK && r.render_pixel_count==64 && r.render_sequence==frame);
            for(unsigned i=0;i<64;++i)configASSERT(r.render_pixels[i]==(0xff000000u|(frame<<12)|(first+i)));
        }
        if(gpu_render_case==3) {
            make_vertices(3);
            configASSERT(!pvr_gpu_service_draw_next(&service,1,PVR_RENDER_DRAW_MAX_FRAMES,source,96,3,1000000,0,&ticket));
            uint64_t saved=service.clients[0].next_ticket;service.clients[0].next_ticket=UINT64_MAX;
            configASSERT(!pvr_gpu_service_draw_next(&service,1,1,source,96,3,1000000,0,&ticket));
            service.clients[0].next_ticket=saved;
            for(unsigned fault=0;fault<3;++fault) {
                uintptr_t timeline=a733_gpu_graphics_memory_render_shared(&graphics)+1412;
                if(fault==0)__atomic_store_n((uint32_t *)timeline,0,__ATOMIC_RELEASE);
                if(fault==1)graphics.submission.geometry.serial=graphics.submission.fragment.serial=UINT64_MAX;
                if(fault==2)graphics.submission.geometry.context=&graphics;
                configASSERT(pvr_gpu_service_draw_next(&service,1,1,source,96,3,1000000,0,&ticket));
                configASSERT(pvr_gpu_service_collect(&service,ticket,&r,1000) && r.result==PVR_GPU_SERVICE_INVALID && r.render_sequence==1);
                configASSERT(!atomic_load(&graphics.submission.draw_state) && graphics.submission.phase==PVR_RENDER_SERVICE_COMPLETE);
                if(fault==0)__atomic_store_n((uint32_t *)timeline,1,__ATOMIC_RELEASE);
                if(fault==1)graphics.submission.geometry.serial=graphics.submission.fragment.serial=1;
                if(fault==2)graphics.submission.geometry.context=&graphics.submission;
                configASSERT(!memcmp((void *)draw_input(),expected_vertices,126*32));
                configASSERT(pvr_gpu_service_render_read_sequence(&service,1,0,0,&ticket));
                configASSERT(pvr_gpu_service_collect(&service,ticket,&r,1000) && r.result==PVR_GPU_SERVICE_OK && r.render_pixels[0]==0xff001000u);
            }
            break;
        }
    }
    if(gpu_render_case!=3)configASSERT(geometry_wrapped && fragment_wrapped && graphics.submission.geometry.serial==12 && graphics.submission.fragment.serial==12);
    r=request(PVR_GPU_SERVICE_CLOSE,1000000);configASSERT(r.result==PVR_GPU_SERVICE_OK);
    for(unsigned i=0;i<2000;++i){vTaskDelay(1);r=request(PVR_GPU_SERVICE_STATUS,0);if(r.phase==PVR_GPU_SERVICE_STOPPED)break;configASSERT(r.phase!=PVR_GPU_SERVICE_FAILED);}
    configASSERT(r.phase==PVR_GPU_SERVICE_STOPPED && isolated && !running);
    platform_puts(gpu_render_case==3?"PASS repeated draw: frame/ticket/serial bounds, timeline and context refusals preserve prior output\n":
        "PASS repeated draw: twelve changing frames, both rings wrap, alternating RT state, exact copied outputs and healthy CLOSE\n");platform_exit(0);
}
void kernel_main(void)
{
    platform_init();configASSERT(gpu_render_case<4);
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
