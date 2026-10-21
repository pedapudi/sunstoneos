#include "pvr_render_service.h"
#include "a733_gpu_graphics_memory.h"
#include "aarch64_device_memory.h"
#include <string.h>

void pvr_render_service_abort(struct pvr_render_service *s, unsigned error)
{
    if (!s || s->phase == PVR_RENDER_SERVICE_NEW || s->phase == PVR_RENDER_SERVICE_FAILED) return;
    s->error=error;s->phase=PVR_RENDER_SERVICE_FAILED;
}
static int failed(struct pvr_render_service *s,unsigned error)
{
    pvr_render_service_abort(s,error);
    if(s->geometry.phase) pvr_cccb_abort(&s->geometry);
    if(s->fragment.phase) pvr_cccb_abort(&s->fragment);
    return -1;
}
static int admitted(struct pvr_render_service *s)
{
    return s && s->owner && &s->owner->submission==s &&
        s->phase!=PVR_RENDER_SERVICE_FAILED &&
        a733_gpu_graphics_memory_render_admitted(s->owner) &&
        s->owner->phase==A733_GPU_GRAPHICS_READY &&
        s->owner->vm.phase==A733_GPU_VM_MEMORY_PUBLISHED &&
        s->owner->service->config.render==s &&
        s->owner->service->config.graphics==s->owner &&
        (s->owner->service->phase==PVR_GPU_SERVICE_READY ||
         (s->phase==PVR_RENDER_SERVICE_CLEANUP &&
          s->owner->service->phase==PVR_GPU_SERVICE_GRAPHICS_CLEANUP)) &&
        s->owner->service->queue.phase==PVR_CCB_ACTIVE &&
        s->owner->service->epoch==s->epoch;
}
static int now(void *context,uint64_t *out)
{
    struct pvr_render_service *s=context;
    if (!admitted(s)) return 0;
    /* The service clock also orders this sample after both rings and the pair. */
    if(pvr_gpu_service_now(s->owner->service,out)!=1) return 0;
    s->last_now=*out;return 1;
}
static int mapped(uintptr_t cpu,size_t bytes,unsigned attr)
{
    if(!bytes || cpu>UINTPTR_MAX-bytes) return 0;
    uintptr_t end=(cpu+bytes-1)&~(uintptr_t)4095;
    for(uintptr_t p=cpu&~(uintptr_t)4095;;p+=4096) {
        struct aarch64_translation r,w;
        if(!aarch64_device_translate(p,0,&r) || !aarch64_device_translate(p,1,&w) ||
            r.physical!=p || w.physical!=p || r.attributes!=attr || w.attributes!=attr ||
            r.shareability!=w.shareability || (r.shareability!=3 && (attr!=0x44 || r.shareability!=2))) return 0;
        if(p==end)return 1;
    }
}
static int transfer(void *context,unsigned fragment,enum pvr_cccb_region region,
    uint32_t offset,void *data,size_t bytes,int write)
{
    struct pvr_render_service *s=context;
    if(!admitted(s) || !data || !bytes || bytes>256 || (offset&3) || (bytes&3) ||
        (region!=PVR_CCCB_CONTROL && region!=PVR_CCCB_COMMANDS)) return 0;
    size_t limit=region==PVR_CCCB_CONTROL?32:2048;
    if(offset>limit || bytes>limit-offset)return 0;
    uintptr_t base=a733_gpu_graphics_memory_render_shared(s->owner);
    base+=(region==PVR_CCCB_CONTROL?(fragment?1344:1280):(fragment?8192:4096))+offset;
    if(!mapped(base,bytes,0x44))return 0;
    for(size_t i=0;i<bytes;i+=4) {
        uint32_t v;
        if(write){memcpy(&v,(const unsigned char *)data+i,4);__atomic_store_n((uint32_t *)(base+i),v,__ATOMIC_RELEASE);}
        else {v=__atomic_load_n((uint32_t *)(base+i),__ATOMIC_ACQUIRE);memcpy((unsigned char *)data+i,&v,4);}
    }
    return 1;
}
static int geom_read(void *s,enum pvr_cccb_region r,uint32_t o,void *p,size_t n)
{ return transfer(s,0,r,o,p,n,0); }
static int geom_write(void *s,enum pvr_cccb_region r,uint32_t o,const void *p,size_t n)
{ return transfer(s,0,r,o,(void *)p,n,1); }
static int frag_read(void *s,enum pvr_cccb_region r,uint32_t o,void *p,size_t n)
{ return transfer(s,1,r,o,p,n,0); }
static int frag_write(void *s,enum pvr_cccb_region r,uint32_t o,const void *p,size_t n)
{ return transfer(s,1,r,o,(void *)p,n,1); }
static int barrier(void *s,enum pvr_ccb_barrier kind)
{ (void)kind;if(!admitted(s))return 0;__asm__ volatile("dsb sy":::"memory");return 1; }
int pvr_render_service_cleanup_prepare(struct pvr_render_service *s,uint64_t deadline)
{
    if(!s || !s->owner || (s->phase!=PVR_RENDER_SERVICE_READY && s->phase!=PVR_RENDER_SERVICE_COMPLETE) ||
       atomic_load_explicit(&s->draw_state,memory_order_acquire) ||
       atomic_load_explicit(&s->texture_state,memory_order_acquire) ||
       atomic_load_explicit(&s->texture_output_state,memory_order_acquire) ||
       s->texture_draft || s->texture_preparing)return 0;
    struct pvr_gpu_service *g=s->owner->service;
    if(s->phase==PVR_RENDER_SERVICE_READY)s->epoch=g->epoch;
    uint64_t clock;
    if(!admitted(s) || !now(s,&clock) || deadline<=clock || deadline!=g->deadline)return 0;
    uintptr_t timeline=a733_gpu_graphics_memory_render_shared(s->owner)+1408;
    if(!mapped(timeline,8,0x44))return 0;
    uint32_t fragment=__atomic_load_n((uint32_t *)(timeline+4),__ATOMIC_ACQUIRE);
    uint32_t geometry=__atomic_load_n((uint32_t *)timeline,__ATOMIC_ACQUIRE);
    if(s->phase==PVR_RENDER_SERVICE_COMPLETE) {
        uint32_t checked=PVR_RENDER_OUTPUT_BYTES;
        if(s->owner->draw>=2) {
            struct pvr_render_texture_layout l;
            if(!pvr_render_texture_layout_profile(s->owner->profile.render.width,s->owner->profile.render.height,s->owner->draw,&l))return 0;
            checked=l.output_bytes;
        }
        if(!s->sequence || s->sequence>UINT32_MAX/2 || geometry!=s->sequence || fragment!=s->sequence*2 ||
           s->checked!=checked || !pvr_cccb_pair_reusable(&s->pair) || !s->pair.fragment_tail ||
           s->pair.geometry!=&s->geometry || s->pair.fragment!=&s->fragment ||
           s->geometry.queue!=&g->queue || s->geometry.context!=s || s->fragment.context!=s ||
           s->geometry.serial!=s->sequence || s->fragment.serial!=(uint64_t)s->sequence*2 ||
           s->geometry.firmware_context!=s->owner->render_object.address ||
           s->fragment.firmware_context!=s->owner->render_object.address+152)return 0;
    } else {
        if(s->sequence || geometry || fragment || s->geometry.phase!=PVR_CCCB_NEW ||
           s->fragment.phase!=PVR_CCCB_NEW)return 0;
        const struct pvr_cccb_ops geom={now,geom_read,geom_write,barrier},frag={now,frag_read,frag_write,barrier};
        if(!pvr_cccb_init(&s->geometry,&geom,s,&g->queue,2048,s->owner->render_object.address,deadline) ||
           !pvr_cccb_init(&s->fragment,&frag,s,&g->queue,2048,s->owner->render_object.address+152,deadline))
            return failed(s,PVR_RENDER_SERVICE_QUEUE);
    }
    s->deadline=deadline;s->phase=PVR_RENDER_SERVICE_CLEANUP;return 1;
}
int pvr_render_service_cleanup_format(struct pvr_render_service *s)
{
    uint64_t clock;
    if(!s || s->phase!=PVR_RENDER_SERVICE_CLEANUP || !now(s,&clock) || clock>=s->deadline)return -1;
    struct pvr_cccb *ring=s->fragment.phase==PVR_CCCB_FORMAT?&s->fragment:
        s->geometry.phase==PVR_CCCB_FORMAT?&s->geometry:NULL;
    if(ring)return pvr_cccb_poll(ring)==PVR_CCCB_OK?0:failed(s,PVR_RENDER_SERVICE_QUEUE);
    return s->geometry.phase==PVR_CCCB_IDLE && s->fragment.phase==PVR_CCCB_IDLE?1:-1;
}
int pvr_render_service_begin(struct pvr_render_service *s,uint64_t deadline)
{
    if(!s || s->phase!=PVR_RENDER_SERVICE_READY || !s->owner || (s->owner->draw && !s->draw_accepted)) return 0;
    struct pvr_gpu_service *g=s->owner->service;
    s->epoch=g->epoch;
    uint64_t clock;
    if(!admitted(s) || !now(s,&clock) || deadline<=clock ||
        pvr_compute_service_admit_mode(g->config.compute)!=1) return 0;
    uintptr_t shared=a733_gpu_graphics_memory_render_shared(s->owner);
    if(!mapped(shared+1408,8,0x44) || __atomic_load_n((uint32_t *)(shared+1408),__ATOMIC_ACQUIRE) ||
        __atomic_load_n((uint32_t *)(shared+1412),__ATOMIC_ACQUIRE)) return 0;
    static const struct pvr_cccb_ops geom={now,geom_read,geom_write,barrier};
    static const struct pvr_cccb_ops frag={now,frag_read,frag_write,barrier};
    s->deadline=deadline;
    if(!pvr_cccb_init(&s->geometry,&geom,s,&g->queue,2048,A733_GPU_RENDER_FW,deadline) ||
        !pvr_cccb_init(&s->fragment,&frag,s,&g->queue,2048,A733_GPU_RENDER_FW+152,deadline)) {
        failed(s,PVR_RENDER_SERVICE_QUEUE);return 0;
    }
    s->phase=PVR_RENDER_SERVICE_FORMAT_GEOMETRY;
    s->sequence=1;
    if(!now(s,&clock) || clock>=deadline){failed(s,PVR_RENDER_SERVICE_CLOCK);return 0;}
    return 1;
}
int pvr_render_service_draw_next(struct pvr_render_service *s,uint32_t previous,
    const void *vertices,size_t bytes,uint32_t count,uint64_t deadline)
{
    if(!s || !s->owner || s->owner->draw!=1 || previous!=s->sequence || previous>=PVR_RENDER_DRAW_MAX_FRAMES ||
        (previous ? (s->phase!=PVR_RENDER_SERVICE_COMPLETE || s->checked!=PVR_RENDER_OUTPUT_BYTES ||
            s->pair.geometry!=&s->geometry || s->pair.fragment!=&s->fragment ||
            s->geometry.queue!=&s->owner->service->queue || s->fragment.queue!=&s->owner->service->queue ||
            s->geometry.context!=s || s->fragment.context!=s ||
            s->geometry.firmware_context!=A733_GPU_RENDER_FW || s->fragment.firmware_context!=A733_GPU_RENDER_FW+152 ||
            !pvr_cccb_pair_reusable(&s->pair) || !s->pair.fragment_tail ||
            s->fragment.serial!=(uint64_t)previous*2 || s->geometry.serial!=previous) :
            (s->phase!=PVR_RENDER_SERVICE_READY || s->draw_accepted)) ||
        !pvr_render_draw_vertices_valid(vertices,bytes,count))return 0;
    s->epoch=s->owner->service->epoch;
    uint64_t clock;
    if(!admitted(s) || !now(s,&clock) || deadline<=clock ||
        pvr_compute_service_admit_mode(s->owner->service->config.compute)!=1)return 0;
    uintptr_t timelines=a733_gpu_graphics_memory_render_shared(s->owner)+1408;
    if(!mapped(timelines,8,0x44))return 0;
    uint32_t fragment=__atomic_load_n((uint32_t *)(timelines+4),__ATOMIC_ACQUIRE);
    uint32_t geometry=__atomic_load_n((uint32_t *)timelines,__ATOMIC_ACQUIRE);
    if(fragment!=previous*2u || geometry!=previous)return 0;
    uint32_t sequence=previous+1;
    struct pvr_render_job_image image;
    uint32_t hwrt=s->owner->target.data_firmware[(sequence-1)&1];
    if(!pvr_render_job_build_frame_pr(&s->programs,hwrt,A733_GPU_RENDER_FW+1408,A733_GPU_RENDER_FW+1412,sequence,&image))return 0;
    uint64_t scissor=PVR_RENDER_DRAW_SCISSOR;memcpy(image.fragment+112,&scissor,8);
    uintptr_t payload=s->owner->payload_extent.cpu+s->owner->render.payload_bytes;
    uintptr_t input=payload+PVR_RENDER_PROGRAM_BYTES+4096u;
    uintptr_t commands=payload+8192+0xf000;
    if(!mapped(input,4096,0xff) || !mapped(commands,4096,0xff))return failed(s,PVR_RENDER_SERVICE_MEMORY);
    memset((void *)input,0,4096);memcpy((void *)input,vertices,bytes);
    memcpy((void *)(commands+0x20),&count,4);
    /* Programs/PPP/metadata were published before OPEN. Runtime changes only
     * this input page and the VDM count; publish both through their EL1 mapping. */
    if(!aarch64_device_cache(AARCH64_CACHE_CLEAN,input,4096) ||
        !aarch64_device_cache(AARCH64_CACHE_CLEAN,commands,4096))return failed(s,PVR_RENDER_SERVICE_MEMORY);
    uintptr_t output=payload+8192+PVR_RENDER_OUTPUT_OFFSET;
    for(unsigned page=0;page<5;++page) {
        if(!now(s,&clock) || clock>=deadline)return failed(s,PVR_RENDER_SERVICE_CLOCK);
        uintptr_t at=output+page*4096u;
        if(!mapped(at,4096,0xff))return failed(s,PVR_RENDER_SERVICE_MEMORY);
        for(unsigned i=0;i<4096;i+=4) {
            unsigned offset=page*4096u+i;
            uint32_t value=offset<64 || offset>=PVR_RENDER_OUTPUT_BYTES-64?PVR_RENDER_GUARD:PVR_RENDER_POISON;
            memcpy((void *)(at+i),&value,4);
        }
        if(!aarch64_device_cache(AARCH64_CACHE_CLEAN_INVALIDATE,at,4096))return failed(s,PVR_RENDER_SERVICE_MEMORY);
    }
    __asm__ volatile("dsb sy":::"memory");
    s->draw_accepted=1;
    s->image=image;s->checked=0;
    if(!previous) {
        if(!pvr_render_service_begin(s,deadline))return failed(s,PVR_RENDER_SERVICE_ADMISSION);
    } else {s->sequence=sequence;s->deadline=deadline;s->phase=PVR_RENDER_SERVICE_SUBMIT;}
    return 1;
}
int pvr_render_service_draw(struct pvr_render_service *s,const void *v,size_t bytes,uint32_t count,uint64_t deadline)
{ return pvr_render_service_draw_next(s,0,v,bytes,count,deadline); }
int pvr_render_service_texture_request(struct pvr_render_service *s,uint64_t deadline)
{
    if(!s || !s->owner || s->owner->draw<2)return 0;
    s->epoch=s->owner->service->epoch;
    uint64_t clock;struct pvr_render_texture_layout l;
    if(!admitted(s) || !now(s,&clock) || clock>=deadline ||
        !pvr_render_texture_layout_profile(s->owner->profile.render.width,s->owner->profile.render.height,s->owner->draw,&l) ||
        pvr_compute_service_admit_mode(s->owner->service->config.compute)!=1)return 0;
    const struct pvr_render_texture_request *q=&s->texture_request;
    if(q->operation!=PVR_TEXTURE_BEGIN && q->state)return 0;
    uintptr_t base=s->owner->payload_extent.cpu+s->owner->render.payload_bytes;
    unsigned held=atomic_load_explicit(&s->texture_output_state,memory_order_acquire);
    if(q->operation==PVR_TEXTURE_RELEASE) {
        if(held!=1 || q->generation!=s->texture_output_ticket || q->previous!=s->texture_output_frame ||
            s->texture_client!=s->texture_output_client || q->offset!=s->texture_output_offset || q->bytes)return 0;
        atomic_store_explicit(&s->texture_output_state,0,memory_order_release);return 1;
    }
    if(held || q->previous!=s->sequence)return 0;
    if(q->operation==PVR_TEXTURE_READ) {
        if(s->phase!=PVR_RENDER_SERVICE_COMPLETE || !q->previous || q->generation ||
            !q->bytes || q->bytes>PVR_RENDER_TEXTURE_CHUNK || (q->offset&3) || (q->bytes&3) ||
            q->offset>=l.output_bytes-128 || q->bytes>l.output_bytes-128-q->offset || s->checked!=l.output_bytes)return 0;
        if(!mapped(base+l.copy+q->offset,q->bytes,0xff) || !mapped(base+l.download,q->bytes,0xff))
            return failed(s,PVR_RENDER_SERVICE_MEMORY);
        memcpy((void *)(base+l.download),(void *)(base+l.copy+q->offset),q->bytes);
        if(!now(s,&clock) || clock>=deadline)return failed(s,PVR_RENDER_SERVICE_CLOCK);
        s->texture_output_ticket=s->texture_ticket;s->texture_output_client=s->texture_client;
        s->texture_output_frame=q->previous;s->texture_output_offset=q->offset;s->texture_output_bytes=q->bytes;
        atomic_store_explicit(&s->texture_output_state,1,memory_order_release);return 1;
    }
    if(q->operation==PVR_TEXTURE_BEGIN) {
        uint32_t stride,bytes;
        if(q->generation || q->offset || q->bytes || !pvr_render_texture_state_valid(s->owner->draw,q->state) || s->sequence>=PVR_RENDER_DRAW_MAX_FRAMES ||
            s->texture_generation==UINT64_MAX ||
            !pvr_render_texture_geometry(q->input.width,q->input.height,&stride,&bytes) || q->input.stride!=stride ||
            !q->input.vertex_count || q->input.vertex_count>PVR_RENDER_TEXTURE_VERTICES ||
            q->input.index_count<3 || q->input.index_count>PVR_RENDER_TEXTURE_INDICES || q->input.index_count%3)return 0;
        s->texture_input=q->input;s->texture_render_state=q->state;s->texture_total=q->input.vertex_count*24+q->input.index_count*2+bytes;
        s->texture_received=0;s->texture_previous=q->previous;++s->texture_generation;
        s->texture_draft=1;s->texture_draft_deadline=deadline;return 1;
    }
    if(!s->texture_draft || q->generation!=s->texture_generation || q->previous!=s->texture_previous)return 0;
    if(clock>=s->texture_draft_deadline){s->texture_draft=0;return 0;}
    if(q->operation==PVR_TEXTURE_WRITE) {
        if(!q->bytes || q->bytes>PVR_RENDER_TEXTURE_CHUNK || q->offset!=s->texture_received ||
            q->offset>s->texture_total || q->bytes>s->texture_total-q->offset)return 0;
        if(!mapped(base+l.draft+q->offset,q->bytes,0xff) || !mapped(base+l.upload,q->bytes,0xff))
            return failed(s,PVR_RENDER_SERVICE_MEMORY);
        memcpy((void *)(base+l.draft+q->offset),(void *)(base+l.upload),q->bytes);
        if(!now(s,&clock) || clock>=deadline || clock>=s->texture_draft_deadline)return 0;
        s->texture_received+=q->bytes;return 1;
    }
    if(q->operation!=PVR_TEXTURE_SUBMIT || q->offset || q->bytes || s->texture_received!=s->texture_total ||
        s->sequence>=PVR_RENDER_DRAW_MAX_FRAMES)return 0;
    uint32_t vertices=s->texture_input.vertex_count*24,indices=s->texture_input.index_count*2;
    if(!mapped(base+l.draft,s->texture_total,0xff) ||
        !pvr_render_texture_state_valid(s->owner->draw,s->texture_render_state) ||
        !pvr_render_texture_input_valid_profile(s->owner->draw,&s->texture_input,(void *)(base+l.draft),(void *)(base+l.draft+vertices),
            s->texture_total-vertices-indices))return 0;
    uint32_t previous=s->sequence;
    if(previous ? (s->phase!=PVR_RENDER_SERVICE_COMPLETE || s->checked!=l.output_bytes ||
        s->pair.geometry!=&s->geometry || s->pair.fragment!=&s->fragment ||
        s->geometry.queue!=&s->owner->service->queue || s->fragment.queue!=&s->owner->service->queue ||
        s->geometry.context!=s || s->fragment.context!=s ||
        s->geometry.firmware_context!=A733_GPU_RENDER_FW || s->fragment.firmware_context!=A733_GPU_RENDER_FW+152 ||
        !pvr_cccb_pair_reusable(&s->pair) || !s->pair.fragment_tail ||
            s->fragment.serial!=(uint64_t)previous*2 || s->geometry.serial!=previous) :
        (s->phase!=PVR_RENDER_SERVICE_READY || s->draw_accepted))return 0;
    uintptr_t timeline=a733_gpu_graphics_memory_render_shared(s->owner)+1408;
    if(!mapped(timeline,8,0x44))return 0;
    uint32_t fragment=__atomic_load_n((uint32_t *)(timeline+4),__ATOMIC_ACQUIRE);
    uint32_t geometry=__atomic_load_n((uint32_t *)timeline,__ATOMIC_ACQUIRE);
    if(fragment!=previous*2u || geometry!=previous)return 0;
    struct pvr_render_job_image image;uint32_t next=previous+1;
    if(!pvr_render_job_build_frame_pr(&s->programs,s->owner->target.data_firmware[(next-1)&1],
        A733_GPU_RENDER_FW+1408,A733_GPU_RENDER_FW+1412,next,&image))return 0;
    uint64_t scissor=PVR_RENDER_DRAW_SCISSOR;memcpy(image.fragment+112,&scissor,8);
    if(s->owner->draw==3 && !pvr_render_texture_depth_job(s->owner->profile.render.width,
        s->owner->profile.render.height,&image))return 0;
    if(!now(s,&clock) || clock>=deadline || clock>=s->texture_draft_deadline)return 0;
    /* Acceptance revokes the old snapshot before any active buffer changes.
     * Only this worker mutates the draft, commands and mapped backing. */
    s->image=image;s->sequence=next;s->checked=0;s->deadline=deadline;
    s->texture_draft=0;s->texture_cursor=0;s->texture_preparing=1;
    s->phase=PVR_RENDER_SERVICE_TEXTURE_PREPARE;return 1;
}
static int texture_prepare(struct pvr_render_service *s)
{
    struct pvr_render_texture_layout l;
    if(!pvr_render_texture_layout_profile(s->owner->profile.render.width,s->owner->profile.render.height,s->owner->draw,&l))return -1;
    uintptr_t base=s->owner->payload_extent.cpu+s->owner->render.payload_bytes;
    uint32_t vertices=s->texture_input.vertex_count*24,indices=s->texture_input.index_count*2;
    uint32_t texture=s->texture_total-vertices-indices;
    uint32_t total=l.draft-l.output;
    for(unsigned batch=0;batch<16 && s->texture_cursor<total; ++batch,s->texture_cursor+=4096) {
        uint64_t clock;
        if(!now(s,&clock) || clock>=s->deadline)return failed(s,PVR_RENDER_SERVICE_CLOCK);
        uint32_t offset=l.output+s->texture_cursor;uintptr_t at=base+offset;
        if(!mapped(at,4096,0xff))return failed(s,PVR_RENDER_SERVICE_MEMORY);
        if(offset<l.image) {
            for(unsigned i=0;i<4096;i+=4) {
                uint32_t relative=offset-l.output+i;
                uint32_t value=relative<64 || relative>=l.output_bytes-64?PVR_RENDER_GUARD:PVR_RENDER_POISON;
                memcpy((void *)(at+i),&value,4);
            }
        } else {
            uint32_t relative,available;uintptr_t source;
            if(l.depth && offset>=l.depth) {
                uint32_t value=offset==l.depth || offset==l.depth+PVR_RENDER_DEPTH_BYTES+4096?PVR_RENDER_GUARD:0x3f800000;
                for(unsigned i=0;i<4096;i+=4)memcpy((void *)(at+i),&value,4);
                if(!aarch64_device_cache(AARCH64_CACHE_CLEAN_INVALIDATE,at,4096))return failed(s,PVR_RENDER_SERVICE_MEMORY);
                continue;
            }
            if(offset<l.geometry){relative=offset-l.image;available=texture;source=base+l.draft+vertices+indices;}
            else if(offset<l.geometry+PVR_RENDER_TEXTURE_VERTEX_BYTES) {
                relative=offset-l.geometry;available=vertices;source=base+l.draft;
            } else {relative=offset-l.geometry-PVR_RENDER_TEXTURE_VERTEX_BYTES;available=indices;source=base+l.draft+vertices;}
            unsigned n=relative<available?available-relative:0;if(n>4096)n=4096;
            memset((void *)at,0,4096);
            if(n) {
                if(!mapped(source+relative,n,0xff))return failed(s,PVR_RENDER_SERVICE_MEMORY);
                memcpy((void *)at,(void *)(source+relative),n);
            }
        }
        if(!aarch64_device_cache(offset<l.image?AARCH64_CACHE_CLEAN_INVALIDATE:AARCH64_CACHE_CLEAN,at,4096))
            return failed(s,PVR_RENDER_SERVICE_MEMORY);
    }
    if(s->texture_cursor<total)return 0;
    uintptr_t commands=base+8192+0xf000,metadata=base+8192+0x10000;
    if(!mapped(commands,4096,0xff) || !mapped(metadata,4096,0xff) ||
        !pvr_render_texture_descriptor((void *)(metadata+0x100),&s->texture_input))return failed(s,PVR_RENDER_SERVICE_MEMORY);
    memcpy((void *)(base+8192+PVR_RENDER_TEXTURE_COUNT_OFFSET),&s->texture_input.index_count,4);
    if(s->owner->draw==3 && !pvr_render_texture_state_apply((void *)base,s->texture_render_state))
        return failed(s,PVR_RENDER_SERVICE_MEMORY);
    if(!aarch64_device_cache(AARCH64_CACHE_CLEAN,commands,4096) ||
        !aarch64_device_cache(AARCH64_CACHE_CLEAN,metadata,4096))return failed(s,PVR_RENDER_SERVICE_MEMORY);
    __asm__ volatile("dsb sy":::"memory");s->draw_accepted=1;s->texture_preparing=0;
    if(s->sequence==1) {
        s->phase=PVR_RENDER_SERVICE_READY;
        if(!pvr_render_service_begin(s,s->deadline))return failed(s,PVR_RENDER_SERVICE_ADMISSION);
    } else s->phase=PVR_RENDER_SERVICE_SUBMIT;
    return 0;
}
int pvr_render_service_poll(struct pvr_render_service *s)
{
    if(!s || s->phase==PVR_RENDER_SERVICE_FAILED)return -1;
    if(s->phase==PVR_RENDER_SERVICE_COMPLETE)return 1;
    if(s->phase<PVR_RENDER_SERVICE_FORMAT_GEOMETRY || !admitted(s))return failed(s,PVR_RENDER_SERVICE_ADMISSION);
    uint64_t clock;
    if(!now(s,&clock) || clock>=s->deadline)return failed(s,PVR_RENDER_SERVICE_CLOCK);
    switch(s->phase) {
    case PVR_RENDER_SERVICE_TEXTURE_PREPARE:
        if(texture_prepare(s)<0)return -1;
        break;
    case PVR_RENDER_SERVICE_FORMAT_GEOMETRY:
    case PVR_RENDER_SERVICE_FORMAT_FRAGMENT: {
        struct pvr_cccb *client=s->phase==PVR_RENDER_SERVICE_FORMAT_GEOMETRY?&s->geometry:&s->fragment;
        enum pvr_cccb_result result=pvr_cccb_poll(client);
        if(result==PVR_CCCB_FAULT || result==PVR_CCCB_INVALID)return failed(s,PVR_RENDER_SERVICE_QUEUE);
        if(result==PVR_CCCB_OK)++s->phase;
        break;
    }
    case PVR_RENDER_SERVICE_SUBMIT:
        if(!s->pair.phase || (s->pair.phase==3 && s->geometry.serial<s->sequence)) {
            struct pvr_cccb_command geom[]={
                {PVR_CCB_MAGIC|PVR_CCCB_TASK|201,112,s->sequence,s->sequence,s->image.geometry},
                {PVR_CCB_MAGIC|213,8,0,0,s->image.geometry_update}};
            struct pvr_cccb_command frag[]={
                {PVR_CCB_MAGIC|215,8,0,0,s->image.fragment_fence},
                {PVR_CCB_MAGIC|PVR_CCCB_TASK|204,480,s->sequence,s->sequence,s->image.fragment},
                {PVR_CCB_MAGIC|213,8,0,0,s->image.fragment_pr_update}};
            uint32_t cleanup=s->owner->target.data_firmware[(s->sequence-1)&1]+320;
            if(!pvr_cccb_pair_begin(&s->pair,&s->geometry,&s->fragment,geom,2,frag,3,&cleanup,1,NULL,0,s->deadline))
                return failed(s,PVR_RENDER_SERVICE_QUEUE);
        } else {
            int result=pvr_cccb_pair_poll(&s->pair);
            if(result<0)return failed(s,PVR_RENDER_SERVICE_QUEUE);
            if(result==1) {
                if(!s->pair.fragment_tail) {
                    struct pvr_cccb_command final[]={
                        {PVR_CCB_MAGIC|PVR_CCCB_TASK|203,480,s->sequence,s->sequence,s->image.fragment},
                        {PVR_CCB_MAGIC|213,8,0,0,s->image.fragment_update}};
                    uint32_t cleanup=s->owner->target.data_firmware[(s->sequence-1)&1]+320;
                    if(!pvr_cccb_pair_fragment_tail_begin(&s->pair,final,2,&cleanup,1,s->deadline))
                        return failed(s,PVR_RENDER_SERVICE_QUEUE);
                } else s->phase=PVR_RENDER_SERVICE_TIMELINES;
            }
        }
        break;
    case PVR_RENDER_SERVICE_TIMELINES: {
        uintptr_t shared=a733_gpu_graphics_memory_render_shared(s->owner)+1408;
        if(!mapped(shared,8,0x44))return failed(s,PVR_RENDER_SERVICE_MEMORY);
        /* Read the dependent fragment first: geometry may complete between loads.
         * The acquire orders the subsequent prerequisite timeline read. */
        uint32_t b=__atomic_load_n((uint32_t *)(shared+4),__ATOMIC_ACQUIRE);
        uint32_t a=__atomic_load_n((uint32_t *)shared,__ATOMIC_ACQUIRE);
        if(a>s->sequence || b>s->sequence*2u || (b && (b+1u)/2u>a))return failed(s,PVR_RENDER_SERVICE_TIMELINE);
        if(a==s->sequence && b==s->sequence*2u){s->phase=PVR_RENDER_SERVICE_INVALIDATE;s->checked=0;}
        break;
    }
    case PVR_RENDER_SERVICE_INVALIDATE: {
        if(s->owner->draw>=2) {
            struct pvr_render_texture_layout l;
            if(!pvr_render_texture_layout_profile(s->owner->profile.render.width,s->owner->profile.render.height,s->owner->draw,&l))
                return failed(s,PVR_RENDER_SERVICE_MEMORY);
            uintptr_t base=s->owner->payload_extent.cpu+s->owner->render.payload_bytes+l.output;
            uint32_t pages=l.output_allocation/4096,limit=pages+(l.depth?2:0);
            for(unsigned i=0;i<16 && s->checked<limit;++i,++s->checked) {
                uintptr_t at=s->checked<pages?base+s->checked*4096u:
                    base-l.output+l.depth+(s->checked==pages?0:PVR_RENDER_DEPTH_BYTES+4096);
                if(!mapped(at,4096,0xff) || !aarch64_device_cache(AARCH64_CACHE_INVALIDATE,at,4096))
                    return failed(s,PVR_RENDER_SERVICE_MEMORY);
            }
            if(s->checked==limit) {
                if(l.depth)for(unsigned page=0;page<2;++page)for(unsigned i=0;i<4096;i+=4) {
                    uint32_t value;uintptr_t guard=base-l.output+l.depth+(page?PVR_RENDER_DEPTH_BYTES+4096:0);
                    memcpy(&value,(void *)(guard+i),4);if(value!=PVR_RENDER_GUARD)return failed(s,PVR_RENDER_SERVICE_RESULT);
                }
                s->checked=0;s->phase=PVR_RENDER_SERVICE_OUTPUT;
            }
            break;
        }
        uintptr_t output=s->owner->payload_extent.cpu+s->owner->render.payload_bytes+8192+PVR_RENDER_OUTPUT_OFFSET;
        uintptr_t p=output+s->checked*4096u;
        if(!mapped(p,4096,0xff) || !aarch64_device_cache(AARCH64_CACHE_INVALIDATE,p,4096))return failed(s,PVR_RENDER_SERVICE_MEMORY);
        if(++s->checked==5){s->checked=0;s->phase=PVR_RENDER_SERVICE_OUTPUT;}
        break;
    }
    case PVR_RENDER_SERVICE_OUTPUT: {
        if(s->owner->draw>=2) {
            struct pvr_render_texture_layout l;
            if(!pvr_render_texture_layout_profile(s->owner->profile.render.width,s->owner->profile.render.height,s->owner->draw,&l))
                return failed(s,PVR_RENDER_SERVICE_MEMORY);
            uintptr_t base=s->owner->payload_extent.cpu+s->owner->render.payload_bytes;
            uint32_t n=l.output_allocation-s->checked;if(n>PVR_RENDER_TEXTURE_CHUNK)n=PVR_RENDER_TEXTURE_CHUNK;
            if(!mapped(base+l.output+s->checked,n,0xff))return failed(s,PVR_RENDER_SERVICE_MEMORY);
            for(unsigned i=0;i<n;i+=4) {
                uint32_t offset=s->checked+i,value;
                if(offset>=64 && offset<l.output_bytes-64)continue;
                memcpy(&value,(void *)(base+l.output+offset),4);
                if(value!=PVR_RENDER_GUARD)return failed(s,PVR_RENDER_SERVICE_RESULT);
            }
            uint32_t first=s->checked<64?64:s->checked,end=s->checked+n;
            if(end>l.output_bytes-64)end=l.output_bytes-64;
            if(end>first) {
                if(!mapped(base+l.copy+first-64,end-first,0xff))return failed(s,PVR_RENDER_SERVICE_MEMORY);
                memcpy((void *)(base+l.copy+first-64),(void *)(base+l.output+first),end-first);
            }
            s->checked+=n;
            if(s->checked==l.output_allocation){s->checked=l.output_bytes;s->phase=PVR_RENDER_SERVICE_COMPLETE;}
            break;
        }
        uintptr_t output=s->owner->payload_extent.cpu+s->owner->render.payload_bytes+8192+PVR_RENDER_OUTPUT_OFFSET;
        unsigned n=PVR_RENDER_OUTPUT_BYTES-s->checked;if(n>256)n=256;
        if(!mapped(output+s->checked,n,0xff))return failed(s,PVR_RENDER_SERVICE_MEMORY);
        uint32_t values[64];memcpy(values,(void *)(output+s->checked),n);
        for(unsigned i=0;i<n;i+=4) {
            unsigned at=s->checked+i;
            int guard=at<64 || at>=PVR_RENDER_OUTPUT_BYTES-64;
            if((guard && values[i/4]!=PVR_RENDER_GUARD) ||
                (!guard && !s->owner->draw && values[i/4]!=PVR_RENDER_PIXEL))return failed(s,PVR_RENDER_SERVICE_RESULT);
            if(!guard)s->pixels[(at-64)/4]=values[i/4];
        }
        s->checked+=n;
        if(s->checked==PVR_RENDER_OUTPUT_BYTES)s->phase=PVR_RENDER_SERVICE_COMPLETE;
        break;
    }
    default:return failed(s,PVR_RENDER_SERVICE_ADMISSION);
    }
    if(!now(s,&clock) || clock>=s->deadline)return failed(s,PVR_RENDER_SERVICE_CLOCK);
    return s->phase==PVR_RENDER_SERVICE_COMPLETE?1:0;
}
