/* Actual native GPU owner/VM/service under FreeRTOS. Only architectural pool
 * admission and GPU/firmware effects are modeled; no physical authority. */
#define client fixed_render_client
#define kernel_main fixed_render_kernel_main
#define profile fixed_render_profile
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#include "gpu-render.c"
#pragma clang diagnostic pop
#undef profile
#undef client
#undef kernel_main
void kernel_main(void);
static unsigned model_commits,model_ready,stopping,isolated,frame_width,frame_height;
static unsigned char source[PVR_RENDER_TEXTURE_GEOMETRY_BYTES+PVR_RENDER_TEXTURE_MAX_BYTES];
extern int a733_memory_reservations_commit_verified(void);
extern int a733_memory_uncached_commit_verified(void);
int a733_device_memory_ready(void){return model_ready;}
int a733_device_memory_commit_allowed(unsigned nc,uintptr_t start,uintptr_t end)
{
    configASSERT(!model_ready && nc==model_commits);
    configASSERT(start==(nc?UINT64_C(0x78000000):UINT64_C(0x48000000)) &&
        end==(nc?UINT64_C(0x79000000):UINT64_C(0x78000000)));
    ++model_commits;return 1;
}
static uint32_t pixel(unsigned frame,unsigned i)
{return ((graphics.draw==3 && frame==2)?0x80000000u:0xff000000u) | ((frame*17)&255)<<16 | ((i/frame_width)&255)<<8 | ((i%frame_width+frame)&255);}
static int texture_read_reg(void *ctx,uint32_t off,uint32_t width,uint64_t *out)
{
    if(!read_reg(ctx,off,width,out))return 0;
    if(stopping && off!=0x100)*out=off==0x3c8?0x7f:off==0x3898?0xff:0;return 1;
}
static int texture_isolate(void *ctx)
{callback(ctx);configASSERT(stopping && service.queue.phase==PVR_CCB_RETIRED);running=0;isolated=1;return 1;}
static uintptr_t base(void){return graphics.payload_extent.cpu+graphics.render.payload_bytes;}
static void ring(const struct pvr_cccb *client,uintptr_t memory,unsigned start,
    const unsigned *types,unsigned count,uint32_t update)
{
    for(unsigned i=0;i<count;++i) {
        const unsigned char *at=(void *)(memory+start);unsigned type=(unsigned)word(at,4),bytes=(unsigned)word(at+4,4);
        if(type==(PVR_CCB_MAGIC|221)){start=0;at=(void *)memory;type=(unsigned)word(at,4);bytes=(unsigned)word(at+4,4);}
        configASSERT(type==types[i]);
        if((type&0x7fff)==213)configASSERT(word(at+40+4,4)==update);
        start=(start+40+bytes)&(client->size-1);
    }
    configASSERT(start==client->cursor);
}
static uint32_t result_pixel(unsigned frame,unsigned i)
{
    uint32_t color=pixel(frame,i);
    if(graphics.draw==3 && frame==2) {
        const uint32_t background=PVR_RENDER_PIXEL;unsigned alpha=color>>24;uint32_t out=0xff000000;
        for(unsigned c=0;c<3;++c)out|=((((color>>(8*c))&255)*alpha+((background>>(8*c))&255)*(255-alpha)+127)/255)<<(8*c);
        return out;
    }
    return color;
}
static int texture_kick(void *ctx,uint32_t off,uint32_t value)
{
    callback(ctx);configASSERT(running && off==0xb00 && !value);
    unsigned next=(unsigned)word(resource(0),4),slot=(next-1)&127;
    unsigned char *command=resource(1)+slot*88;uint32_t type=(uint32_t)word(command,4);
    uintptr_t shared=a733_gpu_graphics_memory_render_shared(&graphics);
    if(type==(PVR_CCB_MAGIC|107)) {
        configASSERT(!word(resource(PVR_FW_POWER_SYNC),4));put(resource(PVR_FW_POWER_SYNC),4,1);
        if(word(command+8,4)==1)stopping=1;
    } else if(type==(PVR_CCB_MAGIC|117)) {
        unsigned frame=++kicks;configASSERT(graphics.submission.sequence==frame);
        unsigned rt=(frame-1)&1;uint32_t hwrt=graphics.target.data_firmware[rt];
        configASSERT(word(command+20,4)==1 && word(command+24,4)==hwrt+320 && !word(command+52,4));
        const unsigned types[]={PVR_CCB_MAGIC|215,PVR_CCB_MAGIC|PVR_CCCB_TASK|204,PVR_CCB_MAGIC|213};
        ring(&graphics.submission.fragment,shared+8192,(unsigned)word((void *)(shared+1348),4),types,3,2*frame-1);
        configASSERT(word(graphics.submission.image.geometry+88,4)==frame-1 &&
            word(graphics.submission.image.geometry_update+4,4)==frame &&
            word(graphics.submission.image.fragment_fence+4,4)==frame);
        __atomic_store_n((uint32_t *)(shared+1284),graphics.submission.geometry.cursor,__ATOMIC_RELEASE);
        __atomic_store_n((uint32_t *)(shared+1348),graphics.submission.fragment.cursor,__ATOMIC_RELEASE);
        __atomic_store_n((uint32_t *)(shared+1408),frame,__ATOMIC_RELEASE);
        __atomic_store_n((uint32_t *)(shared+1412),frame*2-1,__ATOMIC_RELEASE);
        configASSERT(graphics.submission.phase!=PVR_RENDER_SERVICE_COMPLETE);
    } else if(type==(PVR_CCB_MAGIC|101)) {
        unsigned frame=kicks;configASSERT(word(command+8,4)==A733_GPU_RENDER_FW+152 && word(command+20,4)==1);
        configASSERT(word(command+24,4)==graphics.target.data_firmware[(frame-1)&1]+320);
        const unsigned types[]={PVR_CCB_MAGIC|PVR_CCCB_TASK|203,PVR_CCB_MAGIC|213};
        ring(&graphics.submission.fragment,shared+8192,(unsigned)word((void *)(shared+1348),4),types,2,2*frame);
        if(gpu_render_case==5 && frame==2)return 0; /* An uncertain final kick retains both owners. */
        struct pvr_render_texture_layout l;configASSERT(pvr_render_texture_layout_profile(frame_width,frame_height,graphics.draw,&l));
        uint32_t *output=(void *)(base()+l.output+64),*input=(void *)(base()+l.image);
        configASSERT(word((void *)(base()+8192+PVR_RENDER_TEXTURE_COUNT_OFFSET),4)==6);
        configASSERT(word((void *)(base()+l.geometry+12),4)==0x3f800000);
        configASSERT(word((void *)(base()+l.geometry+PVR_RENDER_TEXTURE_VERTEX_BYTES),2)==0);
        uint32_t ppp=(uint32_t)word((void *)(base()+8192+0xf108),4);
        if(graphics.draw==3) {
            configASSERT(graphics.requirements.table_pages==27 && graphics.vm.tables.page_count==27);
            configASSERT(((ppp>>17)&7)==(frame==1?1:3) && ((ppp>>21)&1)==(frame==2) && ((ppp>>24)&7)==(frame==2));
            configASSERT(word((void *)(base()+8192+0xf114),4)==(frame==1?2304u:2560u));
            configASSERT(word(graphics.submission.image.fragment+144,8)==PVR_RENDER_DEPTH_ADDRESS);
            configASSERT(word(graphics.submission.image.fragment+464,4)&128);
            configASSERT(word((void *)(base()+l.depth+4096),4)==0x3f800000);
        }
        for(unsigned i=0;i<frame_width*frame_height;++i) {
            configASSERT(input[i]==pixel(frame,i) && output[i]==PVR_RENDER_POISON);
            output[i]=result_pixel(frame,i); /* Explicit model; no USC execution. */
        }
        if(gpu_render_case==2 && frame==2)put((void *)(base()+l.depth),4,0);
        __atomic_store_n((uint32_t *)(shared+1348),graphics.submission.fragment.cursor,__ATOMIC_RELEASE);
        __atomic_store_n((uint32_t *)(shared+1412),frame*2,__ATOMIC_RELEASE);
    } else if(type==(PVR_CCB_MAGIC|106))cleanup_record(command,slot);
    else configASSERT(type==PVR_CCB_HEALTH);
    put(resource(0)+128,4,next);put(resource(2)+slot*4,4,1);return 1;
}
static struct pvr_gpu_service_reply texture(struct pvr_render_texture_request *q,const void *data,uint64_t *ticket)
{
    struct pvr_gpu_service_reply r;
    configASSERT(pvr_gpu_service_texture(&service,1,q,data,q->operation==PVR_TEXTURE_BEGIN?30000000:5000000,0,ticket));
    configASSERT(pvr_gpu_service_collect(&service,*ticket,&r,1000));return r;
}
static void invalid_drafts(void)
{
    uint64_t ticket;struct pvr_gpu_service_reply r;
    struct pvr_render_texture_request bad_state={.operation=PVR_TEXTURE_BEGIN,.previous=1,.input={1,1,2,4,6},.state=16};
    r=texture(&bad_state,NULL,&ticket);configASSERT(r.result==PVR_GPU_SERVICE_INVALID && r.render_sequence==1);
    bad_state.state=4;r=texture(&bad_state,NULL,&ticket);configASSERT(r.result==PVR_GPU_SERVICE_INVALID);
    memset(source,0,116);for(unsigned i=0;i<4;++i)put(source+i*24+12,4,0x3f800000);
    uint16_t bad[]={0,1,4,0,1,2};memcpy(source+96,bad,12);
    struct pvr_render_texture_request q={.operation=PVR_TEXTURE_BEGIN,.previous=1,.input={1,1,2,4,6}};
    r=texture(&q,NULL,&ticket);configASSERT(r.result==PVR_GPU_SERVICE_OK);uint64_t generation=r.texture_generation;
    q=(struct pvr_render_texture_request){.operation=PVR_TEXTURE_WRITE,.previous=1,.generation=generation,.bytes=116};
    r=texture(&q,source,&ticket);configASSERT(r.result==PVR_GPU_SERVICE_OK);
    q=(struct pvr_render_texture_request){.operation=PVR_TEXTURE_SUBMIT,.previous=1,.generation=generation};
    r=texture(&q,NULL,&ticket);configASSERT(r.result==PVR_GPU_SERVICE_INVALID && r.render_sequence==1);
    configASSERT(graphics.submission.phase==PVR_RENDER_SERVICE_COMPLETE && !atomic_load(&graphics.submission.texture_state));
    q=(struct pvr_render_texture_request){.operation=PVR_TEXTURE_WRITE,.previous=1,.generation=generation-1,.bytes=4};
    r=texture(&q,source,&ticket);configASSERT(r.result==PVR_GPU_SERVICE_INVALID);
    q=(struct pvr_render_texture_request){.operation=PVR_TEXTURE_BEGIN,.previous=1,.input={1,1,2,4,6}};
    graphics.submission.texture_generation=UINT64_MAX;r=texture(&q,NULL,&ticket);
    configASSERT(r.result==PVR_GPU_SERVICE_INVALID && graphics.submission.texture_generation==UINT64_MAX);
    graphics.submission.texture_generation=generation;
    uint64_t last=service.clients[0].next_ticket;service.clients[0].next_ticket=UINT64_MAX;
    configASSERT(!pvr_gpu_service_texture(&service,1,&q,NULL,1000000,0,&ticket));service.clients[0].next_ticket=last;
    vTaskSuspend(service.worker);
    struct pvr_gpu_service_message message={.request={PVR_GPU_SERVICE_STATUS,0},.ticket=1};
    unsigned filled=0;while(xQueueSend(service.requests,&message,0)==pdTRUE)++filled;
    configASSERT(filled==PVR_GPU_SERVICE_CLIENTS && !pvr_gpu_service_texture(&service,1,&q,NULL,1000000,0,&ticket));
    configASSERT(!atomic_load(&graphics.submission.texture_state) && !service.clients[0].pending);
    for(unsigned i=0;i<filled;++i)configASSERT(xQueueReceive(service.requests,&message,0)==pdTRUE);
    vTaskResume(service.worker);
    /* A fresh valid draft still cannot overwrite backing without both acquired
     * previous timelines. Repairing an invalid draft uses a new generation.
     * Its short bound lets the held draft expire before CLOSE below. */
    put(source+100,2,3);configASSERT(pvr_gpu_service_texture(&service,1,&q,NULL,200000,0,&ticket));
    configASSERT(pvr_gpu_service_collect(&service,ticket,&r,1000) && r.result==PVR_GPU_SERVICE_OK);generation=r.texture_generation;
    q=(struct pvr_render_texture_request){.operation=PVR_TEXTURE_WRITE,.previous=1,.generation=generation,.bytes=116};
    r=texture(&q,source,&ticket);configASSERT(r.result==PVR_GPU_SERVICE_OK);
    uintptr_t timeline=a733_gpu_graphics_memory_render_shared(&graphics)+1412;
    __atomic_store_n((uint32_t *)timeline,0,__ATOMIC_RELEASE);
    q=(struct pvr_render_texture_request){.operation=PVR_TEXTURE_SUBMIT,.previous=1,.generation=generation};
    r=texture(&q,NULL,&ticket);configASSERT(r.result==PVR_GPU_SERVICE_INVALID && r.render_sequence==1);
    __atomic_store_n((uint32_t *)timeline,2,__ATOMIC_RELEASE);
    q=(struct pvr_render_texture_request){.operation=PVR_TEXTURE_READ,.previous=1,.bytes=4};
    r=texture(&q,NULL,&ticket);configASSERT(r.result==PVR_GPU_SERVICE_OK);
    uint64_t read_ticket=ticket;const void *data;uint32_t bytes;
    configASSERT(pvr_gpu_service_texture_output(&service,read_ticket,&data,&bytes) && bytes==4 && word(data,4)==result_pixel(1,0));
    q=(struct pvr_render_texture_request){.operation=PVR_TEXTURE_RELEASE,.previous=1,.generation=read_ticket,.offset=0};
    r=texture(&q,NULL,&ticket);configASSERT(r.result==PVR_GPU_SERVICE_OK);
    /* The held draft keeps CLOSE busy. After its deadline, the next request
     * naming it observes the expiry and discards it without using its bytes. */
    r=request(PVR_GPU_SERVICE_CLOSE,1000000);configASSERT(r.result==PVR_GPU_SERVICE_BUSY && !stopping);
    vTaskDelay(250);
    q=(struct pvr_render_texture_request){.operation=PVR_TEXTURE_WRITE,.previous=1,.generation=generation,.offset=116,.bytes=4};
    r=texture(&q,source,&ticket);configASSERT(r.result==PVR_GPU_SERVICE_INVALID && !graphics.submission.texture_draft);
}
static void texture_client(void *unused)
{
    (void)unused;struct pvr_gpu_service_reply r=request(PVR_GPU_SERVICE_OPEN,15000000);
    configASSERT(r.result==PVR_GPU_SERVICE_OK);
    for(unsigned i=0;i<14000;++i) {vTaskDelay(1);r=request(PVR_GPU_SERVICE_STATUS,0);
        configASSERT(r.result==PVR_GPU_SERVICE_OK);if(r.phase==PVR_GPU_SERVICE_READY)break;}
    configASSERT(r.phase==PVR_GPU_SERVICE_READY && graphics.requirements.table_pages==(graphics.draw==3?27u:17u));
    uint64_t ticket;struct pvr_render_texture_layout l;configASSERT(pvr_render_texture_layout_profile(frame_width,frame_height,graphics.draw,&l));
    for(unsigned frame=1;frame<=2;++frame) {
        unsigned count=(gpu_render_case>=3)?4:65536,vertex_bytes=count*24;
        memset(source,0,vertex_bytes+12);
        for(unsigned i=0;i<count;++i)put(source+i*24+12,4,0x3f800000);
        const uint32_t corners[4][6]={{0xbf800000,0xbf800000,0,0x3f800000,0,0},
            {0x3f800000,0xbf800000,0,0x3f800000,0x3f800000,0},
            {0xbf800000,0x3f800000,0,0x3f800000,0,0x3f800000},
            {0x3f800000,0x3f800000,0,0x3f800000,0x3f800000,0x3f800000}};
        for(unsigned i=0;i<3;++i)memcpy(source+i*24,corners[i],24);
        memcpy(source+(count-1)*24,corners[3],24);
        if(graphics.draw==3)for(unsigned i=0;i<count;++i)put(source+i*24+8,4,frame==1?0x3e800000:0x3f000000);
        uint16_t indices[]={0,1,(uint16_t)(count-1),0,(uint16_t)(count-1),2};memcpy(source+vertex_bytes,indices,12);
        for(unsigned i=0;i<frame_width*frame_height;++i)put(source+vertex_bytes+12+i*4,4,pixel(frame,i));
        struct pvr_render_texture_request q={.operation=PVR_TEXTURE_BEGIN,.previous=frame-1,
            .input={frame_width,frame_height,frame_width,count,6},.state=graphics.draw==3?(frame==1?5:10):0};
        r=texture(&q,NULL,&ticket);configASSERT(r.result==PVR_GPU_SERVICE_OK);
        uint64_t generation=r.texture_generation;
        q=(struct pvr_render_texture_request){.operation=PVR_TEXTURE_SUBMIT,.previous=frame-1,.generation=generation};
        r=texture(&q,NULL,&ticket);configASSERT(r.result==PVR_GPU_SERVICE_INVALID);
        unsigned total=vertex_bytes+12+frame_width*frame_height*4;
        for(unsigned offset=0;offset<total;) {
            unsigned n=total-offset;if(n>PVR_RENDER_TEXTURE_CHUNK)n=PVR_RENDER_TEXTURE_CHUNK;
            q=(struct pvr_render_texture_request){.operation=PVR_TEXTURE_WRITE,.previous=frame-1,
                .generation=generation,.offset=offset,.bytes=n};
            configASSERT(pvr_gpu_service_texture(&service,1,&q,source+offset,1000000,0,&ticket));
            memset(source+offset,0xa5,n); /* Caller storage is no longer borrowed. */
            configASSERT(pvr_gpu_service_collect(&service,ticket,&r,1000) && r.result==PVR_GPU_SERVICE_OK);
            configASSERT(!atomic_load(&graphics.submission.texture_state));offset+=n;
        }
        q=(struct pvr_render_texture_request){.operation=PVR_TEXTURE_SUBMIT,.previous=frame-1,.generation=generation};
        r=texture(&q,NULL,&ticket);configASSERT(r.result==PVR_GPU_SERVICE_OK && r.render_sequence==frame);
        for(unsigned i=0;i<4900;++i) {vTaskDelay(1);r=request(PVR_GPU_SERVICE_STATUS,0);
            if(r.phase==PVR_GPU_SERVICE_FAILED || r.render_phase==PVR_RENDER_SERVICE_COMPLETE)break;}
        if((gpu_render_case==2 || gpu_render_case==5) && frame==2) {
            configASSERT(r.phase==PVR_GPU_SERVICE_FAILED);
            if(gpu_render_case==2)configASSERT(graphics.submission.error==PVR_RENDER_SERVICE_RESULT);
            else configASSERT(graphics.submission.geometry.phase==PVR_CCCB_FAILED && graphics.submission.fragment.phase==PVR_CCCB_FAILED);
            held(firmware.wb_pool,graphics.payload_lease);platform_puts("PASS texture: guard or uncertain tail retains native owners\n");platform_exit(0);
        }
        configASSERT(r.result==PVR_GPU_SERVICE_OK && r.render_phase==PVR_RENDER_SERVICE_COMPLETE && r.render_checked==l.output_bytes);
        for(unsigned offset=0;offset<frame_width*frame_height*4;) {
            unsigned n=frame_width*frame_height*4-offset;if(n>PVR_RENDER_TEXTURE_CHUNK)n=PVR_RENDER_TEXTURE_CHUNK;
            q=(struct pvr_render_texture_request){.operation=PVR_TEXTURE_READ,.previous=frame,.offset=offset,.bytes=n};
            r=texture(&q,NULL,&ticket);configASSERT(r.result==PVR_GPU_SERVICE_OK);
            uint64_t read_ticket=ticket;const void *data;uint32_t bytes;
            configASSERT(pvr_gpu_service_texture_output(&service,read_ticket,&data,&bytes) && bytes==n);
            if(!offset) {
                r=request(PVR_GPU_SERVICE_CLOSE,1000000);configASSERT(r.result==PVR_GPU_SERVICE_BUSY && !stopping);
                q=(struct pvr_render_texture_request){.operation=PVR_TEXTURE_BEGIN,.previous=frame,
                    .input={frame_width,frame_height,frame_width,count,6},.state=graphics.draw==3?(frame==1?5:10):0};
                r=texture(&q,NULL,&ticket);configASSERT(r.result==PVR_GPU_SERVICE_INVALID);
            }
            for(unsigned i=0;i<n;i+=4)configASSERT(word((const unsigned char *)data+i,4)==result_pixel(frame,(offset+i)/4));
            q=(struct pvr_render_texture_request){.operation=PVR_TEXTURE_RELEASE,.previous=frame,
                .generation=read_ticket,.offset=offset};
            r=texture(&q,NULL,&ticket);configASSERT(r.result==PVR_GPU_SERVICE_OK);
            configASSERT(!pvr_gpu_service_texture_output(&service,read_ticket,&data,&bytes));offset+=n;
        }
        q=(struct pvr_render_texture_request){.operation=PVR_TEXTURE_READ,.previous=frame-1,.bytes=4};
        r=texture(&q,NULL,&ticket);configASSERT(r.result==PVR_GPU_SERVICE_INVALID);
        if(gpu_render_case==3){invalid_drafts();break;}
    }
    r=request(PVR_GPU_SERVICE_CLOSE,1000000);configASSERT(r.result==PVR_GPU_SERVICE_OK);
    for(unsigned i=0;i<2000;++i){vTaskDelay(1);r=request(PVR_GPU_SERVICE_STATUS,0);if(r.phase==PVR_GPU_SERVICE_STOPPED)break;}
    configASSERT(r.phase==PVR_GPU_SERVICE_STOPPED && isolated && !running);
    platform_puts(gpu_render_case==3?"PASS texture: invalid indices, generation/ticket bounds, queue-full and missing timeline preserve prior copied output\n":
        "PASS texture: display-sized two inputs, real owner/VM/publication, scoped copied output and healthy CLOSE\n");platform_exit(0);
}
void kernel_main(void)
{
    platform_init();configASSERT(gpu_render_case<6);
    configASSERT(a733_memory_reservations_commit_verified() && a733_memory_uncached_commit_verified());
    configASSERT(model_commits==2);model_ready=1;
    configASSERT(a733_memory_reservations_init() && a733_memory_uncached_init());
    frame_width=gpu_render_case==4?1280:1920;frame_height=gpu_render_case==4?720:1080;
    const struct a733_gpu_graphics_profile profile={{frame_width,frame_height,16384},1024,1024,25,5000000};
    configASSERT(a733_gpu_reserved_prepare(&firmware,9,gpu_firmware,(size_t)(gpu_firmware_end-gpu_firmware),400000000));
    configASSERT(a733_gpu_compute_reserved_prepare(&compute,&firmware,&service,10));
    configASSERT(gpu_render_case==4?
        a733_gpu_graphics_memory_prepare_texture(&graphics,&firmware,&compute,&service,11,&profile):
        a733_gpu_graphics_memory_prepare_depth_blend(&graphics,&firmware,&compute,&service,11,&profile));
    configASSERT(a733_gpu_graphics_memory_validate(&graphics,&firmware.config,&service));
    struct pvr_gpu_service_config *c=&firmware.config;
    c->queue=(struct pvr_ccb_ops){read_queue,write_queue,barrier,texture_kick};
    c->start=(struct pvr_gpu_start_ops){now,read_reg,write_reg,started,publish};
    c->control=(struct pvr_gpu_control_ops){now,irq_read,irq_write,compat,NULL};
    c->stop=(struct pvr_gpu_stop_ops){now,texture_read_reg,write_reg,sync_write,sync_read,texture_isolate,NULL};
    c->enable=enable;c->compute_mode=mode;configASSERT(pvr_gpu_service_init(&service,c,3,1));
    TaskHandle_t t=xTaskCreateStaticAffinitySet(texture_client,"texture-check",4096,NULL,2,client_stack,&client_state,2);
    configASSERT(t && pvr_gpu_service_bind(&service,0,t));vTaskStartScheduler();platform_panic("scheduler",0);
}
