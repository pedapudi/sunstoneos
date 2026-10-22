/* Actual native GPU owner/VM/service under FreeRTOS with graphics profile 4.
 * Only architectural pool admission and GPU/firmware effects are modeled;
 * no physical authority or rasterization is claimed. */
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
static unsigned model_ready,stopping,isolated,frame_width,frame_height;
static unsigned char source[PVR_RENDER_TEXTURE_GEOMETRY_BYTES+PVR_RENDER_TEXTURE_MAX_BYTES+256];
/* READY is modeled: the pools are prepared directly, without the boot record
 * and table-profile checks of a733_device_memory_prepare(). */
int a733_device_memory_ready(void){return model_ready;}
/* Frame states and batch tables submitted by the client; the kick handler
 * decodes the native command stream against them. */
static const uint32_t frames[2]={2u|PVR_RENDER_BATCH_LINEAR|0u<<6|1u<<9,3u|3u<<6|4u<<9|PVR_RENDER_BATCH_SRGB};
static const struct pvr_render_batch tables[2][3]={
    {{0,3,1u|PVR_RENDER_BATCH_DEPTH_WRITE,0},
     {3,3,7u|1u<<6|PVR_RENDER_BATCH_STENCIL|7u<<11|2u<<20,1u|255u<<8|255u<<16}},
    {{0,6,7u|PVR_RENDER_BATCH_NO_COLOR|PVR_RENDER_BATCH_STENCIL|7u<<11|2u<<20,7u|255u<<16},
     {0,6,3u|PVR_RENDER_BATCH_DEPTH_WRITE|1u<<4|PVR_RENDER_BATCH_STENCIL|2u<<11,7u|255u<<8},
     {3,3,7u|4u<<6,0}}};
static uint32_t pixel(unsigned frame,unsigned i)
{return 0xff000000u | ((frame*29)&255)<<16 | ((i/frame_width)&255)<<8 | ((i%frame_width+frame)&255);}
static int batch_read_reg(void *ctx,uint32_t off,uint32_t width,uint64_t *out)
{
    if(!read_reg(ctx,off,width,out))return 0;
    if(stopping && off!=0x100)*out=off==0x3c8?0x7f:off==0x3898?0xff:0;return 1;
}
static int batch_isolate(void *ctx)
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
/* Decode the native stream: one PPP update per batch, shared vertex state
 * after the first, then that batch's index list. */
static void stream(unsigned frame)
{
    const uint32_t *w=(const void *)(base()+8192+PVR_RENDER_BATCH_VDM_OFFSET);
    const struct pvr_render_batch *b=tables[frame-1];unsigned count=frames[frame-1]&31u;
    for(unsigned i=0;i<count;++i) {
        uint64_t block=PVR_RENDER_GENERAL_ADDRESS+PVR_RENDER_BATCH_PPP_OFFSET+i*PVR_RENDER_BATCH_PPP_STRIDE;
        configASSERT(w[0]==(26u<<8|(uint32_t)(block>>32)) && w[1]==(uint32_t)block);w+=2;
        if(!i)w+=PVR_RENDER_BATCH_VDM_STATE_BYTES/4;
        uint64_t index=PVR_RENDER_TEXTURE_INDEX_ADDRESS+b[i].first*2u;
        configASSERT(w[0]==(0x78020600u|(uint32_t)(index>>32)) && w[1]==(uint32_t)index && w[2]==b[i].count);w+=3;
        const uint32_t *ppp=(const void *)(base()+8192+PVR_RENDER_BATCH_PPP_OFFSET+i*PVR_RENDER_BATCH_PPP_STRIDE);
        uint32_t s=b[i].state;unsigned blend=(s>>6)&7;
        configASSERT(((ppp[1]>>21)&1)==!!(s&PVR_RENDER_BATCH_NO_COLOR));
        configASSERT(((ppp[2]>>17)&7)==(s&7) && ((ppp[2]>>21)&1)==!(s&PVR_RENDER_BATCH_DEPTH_WRITE) &&
            ((ppp[2]>>24)&7)==!!blend && (ppp[2]&255)==((s&PVR_RENDER_BATCH_STENCIL)?(b[i].stencil&255):0));
        configASSERT((ppp[24]&3)==((s>>4)&3));
        if(s&PVR_RENDER_BATCH_STENCIL)configASSERT(((ppp[3]>>25)&7)==((s>>11)&7) && ((ppp[3]>>16)&7)==((s>>20)&7) &&
            ((ppp[3]>>8)&255)==((b[i].stencil>>8)&255) && (ppp[3]&255)==((b[i].stencil>>16)&255));
        else configASSERT(ppp[3]==0x0e000000u);
    }
    configASSERT(*w==0xc0000000u);
}
static int batch_kick(void *ctx,uint32_t off,uint32_t value)
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
        configASSERT(word(graphics.submission.image.geometry+16,8)==PVR_RENDER_BATCH_VDM_ADDRESS);
        __atomic_store_n((uint32_t *)(shared+1284),graphics.submission.geometry.cursor,__ATOMIC_RELEASE);
        __atomic_store_n((uint32_t *)(shared+1348),graphics.submission.fragment.cursor,__ATOMIC_RELEASE);
        __atomic_store_n((uint32_t *)(shared+1408),frame,__ATOMIC_RELEASE);
        __atomic_store_n((uint32_t *)(shared+1412),frame*2-1,__ATOMIC_RELEASE);
    } else if(type==(PVR_CCB_MAGIC|101)) {
        unsigned frame=kicks;configASSERT(word(command+8,4)==A733_GPU_RENDER_FW+152 && word(command+20,4)==1);
        const unsigned types[]={PVR_CCB_MAGIC|PVR_CCCB_TASK|203,PVR_CCB_MAGIC|213};
        ring(&graphics.submission.fragment,shared+8192,(unsigned)word((void *)(shared+1348),4),types,2,2*frame);
        struct pvr_render_texture_layout l;configASSERT(pvr_render_texture_layout_profile(frame_width,frame_height,4,&l));
        unsigned x=32,y=32;while(x<frame_width)x*=2;while(y<frame_height)y*=2;
        configASSERT(l.depth_bytes==x*y*8 && graphics.maps[A733_GPU_GRAPHICS_MAPS+5].bytes==l.depth_bytes);
        if(frame_width==1920)configASSERT(graphics.requirements.table_pages==35 && graphics.vm.tables.page_count==35);
        const unsigned char *fragment=graphics.submission.image.fragment;
        configASSERT((word(fragment+464,4)&0x180)==0x180 && word(fragment+468,4)==x*y*8 && word(fragment+472,4)==x*y*8);
        configASSERT(((word(fragment+136,8)>>23)&15)==15 && word(fragment+152,8)==PVR_RENDER_DEPTH_ADDRESS);
        /* Packed depth starts at one with stencil zero between guard pages. */
        configASSERT(word((void *)(base()+l.depth),4)==PVR_RENDER_GUARD &&
            word((void *)(base()+l.depth+4096),4)==0x3f800000 && !word((void *)(base()+l.depth+4100),4) &&
            word((void *)(base()+l.depth+4096+l.depth_bytes-8),4)==0x3f800000 &&
            word((void *)(base()+l.depth+l.depth_bytes+4096),4)==PVR_RENDER_GUARD);
        stream(frame);
        uint64_t sampler=word((void *)(base()+8192+0x10100+32),8),image=word((void *)(base()+8192+0x10100),8);
        configASSERT(((sampler>>36)&15)==((frames[frame-1]&PVR_RENDER_BATCH_LINEAR)?5u:0u));
        configASSERT(((sampler>>41)&7)==(frame==1?0u:4u) && ((sampler>>44)&7)==(frame==1?1u:3u));
        configASSERT(((image>>3)&1)==(frame==2));
        uint32_t *output=(void *)(base()+l.output+64),*input=(void *)(base()+l.image);
        unsigned stride=(frame_width+1)&~1u;
        for(unsigned i=0;i<frame_width*frame_height;++i) {
            unsigned at=(i/frame_width)*stride+i%frame_width;
            configASSERT(input[at]==pixel(frame,i) && output[at]==PVR_RENDER_POISON);
            output[at]=pixel(frame,i)^0x00ffffffu; /* Explicit model; no USC execution. */
        }
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
/* Four corners at w=1 for frame 1. Frame 2 scales each position by w=2 and
 * places z at half depth, so perspective division gives the same corners. */
static unsigned input(unsigned frame,uint32_t state,const struct pvr_render_batch *table)
{
    unsigned count=4,vertex_bytes=count*24,batches=(state&31u)*PVR_RENDER_BATCH_BYTES;
    const float corner[4][2]={{-1,-1},{1,-1},{-1,1},{1,1}};
    for(unsigned i=0;i<count;++i) {
        float w=frame==2?2.0f:1.0f,v[6]={corner[i][0]*w,corner[i][1]*w,frame==2?1.0f:0.0f,w,
            corner[i][0]<0?0.0f:1.0f,corner[i][1]<0?0.0f:1.0f};
        memcpy(source+i*24,v,24);
    }
    uint16_t indices[]={0,1,3,0,3,2};memcpy(source+vertex_bytes,indices,12);
    memcpy(source+vertex_bytes+12,table,batches);
    unsigned stride=(frame_width+1)&~1u;
    memset(source+vertex_bytes+12+batches,0,stride*frame_height*4);
    for(unsigned i=0;i<frame_width*frame_height;++i)
        put(source+vertex_bytes+12+batches+((i/frame_width)*stride+i%frame_width)*4,4,pixel(frame,i));
    return vertex_bytes+12+batches+stride*frame_height*4;
}
static uint64_t upload(unsigned previous,uint32_t state,unsigned total)
{
    uint64_t ticket;struct pvr_render_texture_request q={.operation=PVR_TEXTURE_BEGIN,.previous=previous,
        .input={frame_width,frame_height,(frame_width+1)&~1u,4,6},.state=state};
    struct pvr_gpu_service_reply r=texture(&q,NULL,&ticket);configASSERT(r.result==PVR_GPU_SERVICE_OK);
    uint64_t generation=r.texture_generation;
    for(unsigned offset=0;offset<total;) {
        unsigned n=total-offset;if(n>PVR_RENDER_TEXTURE_CHUNK)n=PVR_RENDER_TEXTURE_CHUNK;
        q=(struct pvr_render_texture_request){.operation=PVR_TEXTURE_WRITE,.previous=previous,
            .generation=generation,.offset=offset,.bytes=n};
        r=texture(&q,source+offset,&ticket);configASSERT(r.result==PVR_GPU_SERVICE_OK);offset+=n;
    }
    return generation;
}
static struct pvr_gpu_service_reply submit(unsigned previous,uint64_t generation)
{
    uint64_t ticket;struct pvr_render_texture_request q={.operation=PVR_TEXTURE_SUBMIT,.previous=previous,.generation=generation};
    return texture(&q,NULL,&ticket);
}
static void readback(unsigned frame)
{
    uint64_t ticket;struct pvr_gpu_service_reply r;unsigned stride=(frame_width+1)&~1u,total=stride*frame_height*4;
    for(unsigned offset=0;offset<total;) {
        unsigned n=total-offset;if(n>PVR_RENDER_TEXTURE_CHUNK)n=PVR_RENDER_TEXTURE_CHUNK;
        struct pvr_render_texture_request q={.operation=PVR_TEXTURE_READ,.previous=frame,.offset=offset,.bytes=n};
        r=texture(&q,NULL,&ticket);configASSERT(r.result==PVR_GPU_SERVICE_OK);
        uint64_t read_ticket=ticket;const void *data;uint32_t bytes;
        configASSERT(pvr_gpu_service_texture_output(&service,read_ticket,&data,&bytes) && bytes==n);
        for(unsigned i=0;i<n;i+=4) {
            unsigned at=(offset+i)/4,x=at%stride,yy=at/stride;
            if(x<frame_width)configASSERT(word((const unsigned char *)data+i,4)==(pixel(frame,yy*frame_width+x)^0x00ffffffu));
        }
        q=(struct pvr_render_texture_request){.operation=PVR_TEXTURE_RELEASE,.previous=frame,.generation=read_ticket,.offset=offset};
        r=texture(&q,NULL,&ticket);configASSERT(r.result==PVR_GPU_SERVICE_OK);offset+=n;
    }
}
static void wait_complete(unsigned frame)
{
    struct pvr_gpu_service_reply r;
    for(unsigned i=0;i<4900;++i){vTaskDelay(1);r=request(PVR_GPU_SERVICE_STATUS,0);
        if(r.phase==PVR_GPU_SERVICE_FAILED || r.render_phase==PVR_RENDER_SERVICE_COMPLETE)break;}
    configASSERT(r.result==PVR_GPU_SERVICE_OK && r.render_phase==PVR_RENDER_SERVICE_COMPLETE && r.render_sequence==frame);
}
/* Each refused draft leaves frame 1 complete and readable. */
static void refused(void)
{
    uint64_t ticket;struct pvr_gpu_service_reply r;
    const uint32_t bad_frames[]={0,17,5u<<6|1,1u<<13|1};
    for(unsigned i=0;i<4;++i) {
        struct pvr_render_texture_request q={.operation=PVR_TEXTURE_BEGIN,.previous=1,
            .input={frame_width,frame_height,(frame_width+1)&~1u,4,6},.state=bad_frames[i]};
        r=texture(&q,NULL,&ticket);configASSERT(r.result==PVR_GPU_SERVICE_INVALID && r.render_sequence==1);
    }
    const struct pvr_render_batch bad[]={{3,6,0,0},{0,4,0,0},{0,3,0,1},{0,3,1u<<6|PVR_RENDER_BATCH_NO_COLOR,0},
        {0,3,5u<<6,0},{0,3,3u<<4,0},{0,3,1u<<23,0}};
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);++i) {
        struct pvr_render_batch table[2]={tables[0][0],bad[i]};
        unsigned total=input(1,frames[0],table);uint64_t generation=upload(1,frames[0],total);
        r=submit(1,generation);configASSERT(r.result==PVR_GPU_SERVICE_INVALID && r.render_sequence==1);
    }
    /* A vertex outside the view volume, then a NaN texture coordinate. */
    for(unsigned i=0;i<2;++i) {
        unsigned total=input(1,frames[0],tables[0]);
        if(i)put(source+16,4,0x7fc00000);else put(source+0,4,0xc0000000);
        uint64_t generation=upload(1,frames[0],total);
        r=submit(1,generation);configASSERT(r.result==PVR_GPU_SERVICE_INVALID && r.render_sequence==1);
    }
    configASSERT(graphics.submission.phase==PVR_RENDER_SERVICE_COMPLETE && graphics.submission.sequence==1);
    readback(1);
    /* A valid replacement draft still renders the next frame. */
    unsigned total=input(2,frames[1],tables[1]);uint64_t generation=upload(1,frames[1],total);
    r=submit(1,generation);configASSERT(r.result==PVR_GPU_SERVICE_OK && r.render_sequence==2);
    wait_complete(2);readback(2);
}
static void present(unsigned frame)
{
    uint64_t ticket;struct pvr_gpu_service_reply r;
    struct pvr_render_texture_request q={.operation=PVR_TEXTURE_PRESENT,.previous=frame,.bytes=4};
    r=texture(&q,NULL,&ticket);configASSERT(r.result==PVR_GPU_SERVICE_INVALID);
    q.bytes=0;q.previous=frame-1;r=texture(&q,NULL,&ticket);configASSERT(r.result==PVR_GPU_SERVICE_INVALID);
    q.previous=frame;r=texture(&q,NULL,&ticket);configASSERT(r.result==PVR_GPU_SERVICE_OK);
    uint64_t lent=ticket;struct pvr_render_texture_frame f;
    configASSERT(!pvr_gpu_service_texture_present(&service,lent+1,&f));
    configASSERT(pvr_gpu_service_texture_present(&service,lent,&f));
    unsigned stride=(frame_width+1)&~1u;struct pvr_render_texture_layout l;
    configASSERT(pvr_render_texture_layout_profile(frame_width,frame_height,4,&l));
    configASSERT(f.cpu==(const void *)(base()+l.copy) && f.physical==(uint64_t)(uintptr_t)f.cpu &&
        f.bytes==stride*frame_height*4 && f.width==frame_width && f.height==frame_height && f.stride==stride && f.frame==frame);
    for(unsigned yy=0;yy<frame_height;++yy)for(unsigned x=0;x<frame_width;++x)
        configASSERT(word((const unsigned char *)f.cpu+(yy*stride+x)*4,4)==(pixel(frame,yy*frame_width+x)^0x00ffffffu));
    /* The lent snapshot excludes READ, replacement and CLOSE. */
    q=(struct pvr_render_texture_request){.operation=PVR_TEXTURE_READ,.previous=frame,.bytes=4};
    r=texture(&q,NULL,&ticket);configASSERT(r.result==PVR_GPU_SERVICE_INVALID);
    q=(struct pvr_render_texture_request){.operation=PVR_TEXTURE_BEGIN,.previous=frame,
        .input={frame_width,frame_height,stride,4,6},.state=frames[0]};
    r=texture(&q,NULL,&ticket);configASSERT(r.result==PVR_GPU_SERVICE_INVALID);
    r=request(PVR_GPU_SERVICE_CLOSE,1000000);configASSERT(r.result==PVR_GPU_SERVICE_BUSY && !stopping);
    q=(struct pvr_render_texture_request){.operation=PVR_TEXTURE_RELEASE,.previous=frame,.generation=lent,.offset=4};
    r=texture(&q,NULL,&ticket);configASSERT(r.result==PVR_GPU_SERVICE_INVALID);
    q.offset=0;r=texture(&q,NULL,&ticket);configASSERT(r.result==PVR_GPU_SERVICE_OK);
    configASSERT(!pvr_gpu_service_texture_present(&service,lent,&f));
}
static void batch_client(void *unused)
{
    (void)unused;struct pvr_gpu_service_reply r=request(PVR_GPU_SERVICE_OPEN,15000000);
    configASSERT(r.result==PVR_GPU_SERVICE_OK);
    for(unsigned i=0;i<14000;++i) {vTaskDelay(1);r=request(PVR_GPU_SERVICE_STATUS,0);
        configASSERT(r.result==PVR_GPU_SERVICE_OK);if(r.phase==PVR_GPU_SERVICE_READY)break;}
    configASSERT(r.phase==PVR_GPU_SERVICE_READY);
    unsigned last=gpu_render_case==2?1:2;
    for(unsigned frame=1;frame<=last;++frame) {
        unsigned total=input(frame,frames[frame-1],tables[frame-1]);
        uint64_t generation=upload(frame-1,frames[frame-1],total);
        r=submit(frame-1,generation);configASSERT(r.result==PVR_GPU_SERVICE_OK && r.render_sequence==frame);
        wait_complete(frame);readback(frame);
        if(gpu_render_case==1){refused();break;}
    }
    if(!gpu_render_case)present(2);
    r=request(PVR_GPU_SERVICE_CLOSE,1000000);configASSERT(r.result==PVR_GPU_SERVICE_OK);
    for(unsigned i=0;i<2000;++i){vTaskDelay(1);r=request(PVR_GPU_SERVICE_STATUS,0);if(r.phase==PVR_GPU_SERVICE_STOPPED)break;}
    configASSERT(r.phase==PVR_GPU_SERVICE_STOPPED && isolated && !running);
    platform_puts(gpu_render_case==1?"PASS batch: invalid frame state, batch fields and vertices preserve prior copied output\n":
        gpu_render_case==2?"PASS batch: small session sizes the packed depth/stencil attachment from its extents\n":
        "PASS batch: two 1080p batch frames, perspective/stencil/sampler state, presented snapshot and healthy CLOSE\n");
    platform_exit(0);
}
void kernel_main(void)
{
    platform_init();configASSERT(gpu_render_case<3);
    configASSERT(!a733_memory_reservations_init() && !a733_memory_uncached_init());
    configASSERT(a733_memory_reservations_prepare() && a733_memory_uncached_prepare());model_ready=1;
    configASSERT(a733_memory_reservations_init() && a733_memory_uncached_init());
    const unsigned widths[]={1920,640,63},heights[]={1080,360,17};
    frame_width=widths[gpu_render_case];frame_height=heights[gpu_render_case];
    const struct a733_gpu_graphics_profile profile={{frame_width,frame_height,16384},1024,1024,25,5000000};
    configASSERT(a733_gpu_reserved_prepare(&firmware,9,gpu_firmware,(size_t)(gpu_firmware_end-gpu_firmware),400000000));
    configASSERT(a733_gpu_compute_reserved_prepare(&compute,&firmware,&service,10));
    configASSERT(a733_gpu_graphics_memory_prepare_batches(&graphics,&firmware,&compute,&service,11,&profile));
    configASSERT(a733_gpu_graphics_memory_validate(&graphics,&firmware.config,&service));
    struct pvr_gpu_service_config *c=&firmware.config;
    c->queue=(struct pvr_ccb_ops){read_queue,write_queue,barrier,batch_kick};
    c->start=(struct pvr_gpu_start_ops){now,read_reg,write_reg,started,publish};
    c->control=(struct pvr_gpu_control_ops){now,irq_read,irq_write,compat,NULL};
    c->stop=(struct pvr_gpu_stop_ops){now,batch_read_reg,write_reg,sync_write,sync_read,batch_isolate,NULL};
    c->enable=enable;c->compute_mode=mode;configASSERT(pvr_gpu_service_init(&service,c,3,1));
    TaskHandle_t t=xTaskCreateStaticAffinitySet(batch_client,"batch-check",4096,NULL,2,client_stack,&client_state,2);
    configASSERT(t && pvr_gpu_service_bind(&service,0,t));vTaskStartScheduler();platform_panic("scheduler",0);
}
