#include "pvr_render_service.h"
#include "a733_gpu_graphics_memory.h"
#include "a733_gpu_mapping.h"
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
{ return a733_gpu_identity_mapped(cpu,bytes,attr,1); }
static int transfer(void *context,unsigned fragment,enum pvr_cccb_region region,
    uint32_t offset,void *data,size_t bytes,int write)
{
    struct pvr_render_service *s=context;
    if(!admitted(s) || !data || !bytes || bytes>256 || (offset&3) || (bytes&3) ||
        (region!=PVR_CCCB_CONTROL && region!=PVR_CCCB_COMMANDS)) return 0;
    size_t limit=region==PVR_CCCB_CONTROL?32:2048;
    if(offset>limit || bytes>limit-offset)return 0;
    /* OPEN checked the shared extent's non-cacheable mapping, and admitted()
     * checks its lease on every call, so no translation is repeated here.
     * Ring words are plain single-copy stores: the client ring's RELEASE
     * barrier orders them before the write offset and the kick. */
    uintptr_t base=a733_gpu_graphics_memory_render_shared(s->owner);
    base+=(region==PVR_CCCB_CONTROL?(fragment?1344:1280):(fragment?8192:4096))+offset;
    for(size_t i=0;i<bytes;i+=4) {
        uint32_t v;
        if(write){memcpy(&v,(const unsigned char *)data+i,4);__atomic_store_n((uint32_t *)(base+i),v,__ATOMIC_RELAXED);}
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
    if(__atomic_load_n((uint32_t *)(shared+1408),__ATOMIC_ACQUIRE) ||
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
/* The payload's write-back mapping is translated once per extent. admitted()
 * checks the payload lease on every clock sample and the platform pins the
 * mapping, so a translation per request would repeat the same answer. */
static int payload_mapped(struct pvr_render_service *s,const struct pvr_render_texture_layout *l)
{
    uintptr_t cpu=s->owner->payload_extent.cpu;size_t bytes=s->owner->render.payload_bytes+(size_t)l->total;
    if(s->mapped_cpu==cpu && s->mapped_bytes==bytes)return 1;
    if(s->owner->payload_extent.bytes!=bytes || !mapped(cpu,bytes,0xff))return 0;
    s->mapped_cpu=cpu;s->mapped_bytes=bytes;return 1;
}
/* No frame is in flight: none was submitted, or the worker completed the last
 * one and both timelines still show it. Only then may input areas change. */
static int idle(const struct pvr_render_service *s)
{
    if((s->phase!=PVR_RENDER_SERVICE_READY && s->phase!=PVR_RENDER_SERVICE_COMPLETE) || s->sequence>UINT32_MAX/2)return 0;
    uintptr_t timeline=a733_gpu_graphics_memory_render_shared(s->owner)+1408;
    uint32_t fragment=__atomic_load_n((uint32_t *)(timeline+4),__ATOMIC_ACQUIRE);
    uint32_t geometry=__atomic_load_n((uint32_t *)timeline,__ATOMIC_ACQUIRE);
    return fragment==s->sequence*2u && geometry==s->sequence;
}
/* Payload words. The image builds with -fno-builtin, so memcpy of four
 * bytes is a call, and the project memcpy moves single bytes unless both
 * addresses share their 8-byte alignment, which upload streams with an odd
 * index count never do. */
typedef uint32_t __attribute__((may_alias)) payload_word;
typedef uint64_t __attribute__((may_alias)) payload_dword;
/* Copy with 8-byte stores. A source that is not 8-byte aligned is read as
 * whole aligned words and shifted; those words stay inside the page-aligned
 * 64 KiB upload chunk that holds the source. */
static void move_bytes(uintptr_t to,uintptr_t from,uint32_t n)
{
    for(;n && (to&7);--n)*(unsigned char *)to++=*(const unsigned char *)from++;
    unsigned shift=(unsigned)(from&7)*8u;
    if(!shift)for(;n>=8;n-=8,to+=8,from+=8)*(payload_dword *)to=*(const payload_dword *)from;
    else {
        const payload_dword *word=(const payload_dword *)(from&~(uintptr_t)7);
        uint64_t low=*word++;
        for(;n>=8;n-=8,to+=8,from+=8) {
            uint64_t high=*word++;
            *(payload_dword *)to=low>>shift | high<<(64u-shift);low=high;
        }
    }
    for(;n;--n)*(unsigned char *)to++=*(const unsigned char *)from++;
}
/* Fill [first, end) of the output allocation; both are multiples of eight. */
static void fill(uintptr_t output,uint32_t first,uint32_t end,uint32_t value)
{
    const uint64_t pair=(uint64_t)value<<32 | value;
    for(uint32_t i=first;i<end;i+=8)*(payload_dword *)(output+i)=pair;
}
/* Maintain whole 64-byte lines around a range inside the payload extent. */
static int lines(enum aarch64_cache_op op,uintptr_t at,size_t bytes)
{
    uintptr_t first=at&~(uintptr_t)63;
    return aarch64_device_cache(op,first,(at+bytes-first+63)&~(size_t)63);
}
/* Each upload byte moves once from the runtime's chunk to its place: the
 * vertex, index or texture area that the GPU reads, cleaned as written, or
 * the worker's private copy of the batch records. */
static int texture_place(struct pvr_render_service *s,const struct pvr_render_texture_layout *l,
    uintptr_t base,uint32_t offset,uint32_t bytes)
{
    const uint32_t vertices=s->texture_input.vertex_count*24,indices=s->texture_input.index_count*2;
    const uint32_t batches=pvr_render_batch_table_bytes(s->owner->draw,s->texture_render_state);
    const uint32_t end[4]={vertices,vertices+indices,vertices+indices+batches,s->texture_total};
    const uintptr_t start[4]={base+l->geometry,base+l->geometry+PVR_RENDER_TEXTURE_VERTEX_BYTES,
        (uintptr_t)s->texture_batches,base+l->image};
    uintptr_t from=base+l->upload;
    for(unsigned part=0;part<4 && bytes;++part) {
        uint32_t first=part?end[part-1]:0;
        if(offset>=end[part])continue;
        uint32_t n=end[part]-offset;if(n>bytes)n=bytes;
        uintptr_t to=start[part]+(offset-first);
        move_bytes(to,from,n);
        if(part!=2 && !lines(AARCH64_CACHE_CLEAN,to,n))return 0;
        from+=n;offset+=n;bytes-=n;
    }
    return 1;
}
int pvr_render_service_texture_request(struct pvr_render_service *s,uint64_t deadline)
{
    if(!s || !s->owner || s->owner->draw<2)return 0;
    s->epoch=s->owner->service->epoch;
    uint64_t clock;struct pvr_render_texture_layout l;
    if(!admitted(s) || !now(s,&clock) || clock>=deadline ||
        !pvr_render_texture_layout_profile(s->owner->profile.render.width,s->owner->profile.render.height,s->owner->draw,&l) ||
        pvr_compute_service_admit_mode(s->owner->service->config.compute)!=1)return 0;
    if(!payload_mapped(s,&l))return failed(s,PVR_RENDER_SERVICE_MEMORY);
    const struct pvr_render_texture_request *q=&s->texture_request;
    if(q->operation!=PVR_TEXTURE_BEGIN && q->state)return 0;
    uintptr_t base=s->owner->payload_extent.cpu+s->owner->render.payload_bytes;
    unsigned held=atomic_load_explicit(&s->texture_output_state,memory_order_acquire);
    if(q->operation==PVR_TEXTURE_RELEASE) {
        if((held!=1 && held!=2) || q->generation!=s->texture_output_ticket || q->previous!=s->texture_output_frame ||
            s->texture_client!=s->texture_output_client || q->offset!=s->texture_output_offset || q->bytes)return 0;
        atomic_store_explicit(&s->texture_output_state,0,memory_order_release);return 1;
    }
    if(held || q->previous!=s->sequence)return 0;
    if(q->operation==PVR_TEXTURE_PRESENT) {
        /* The frame is lent in place, between the guards. The CPU wrote none
         * of it after the clean before the kick and holds no dirty line, so
         * memory holds what the GPU wrote. */
        if(s->phase!=PVR_RENDER_SERVICE_COMPLETE || !q->previous || q->generation || q->offset || q->bytes ||
            s->checked!=l.output_bytes)return 0;
        s->texture_output_ticket=s->texture_ticket;s->texture_output_client=s->texture_client;
        s->texture_output_frame=q->previous;s->texture_output_offset=0;s->texture_output_bytes=l.output_bytes-128;
        atomic_store_explicit(&s->texture_output_state,2,memory_order_release);return 1;
    }
    if(q->operation==PVR_TEXTURE_READ) {
        if(s->phase!=PVR_RENDER_SERVICE_COMPLETE || !q->previous || q->generation ||
            !q->bytes || q->bytes>PVR_RENDER_TEXTURE_CHUNK || (q->offset&3) || (q->bytes&3) ||
            q->offset>=l.output_bytes-128 || q->bytes>l.output_bytes-128-q->offset || s->checked!=l.output_bytes)return 0;
        memcpy((void *)(base+l.download),(void *)(base+l.copy+q->offset),q->bytes);
        if(!now(s,&clock) || clock>=deadline)return failed(s,PVR_RENDER_SERVICE_CLOCK);
        s->texture_output_ticket=s->texture_ticket;s->texture_output_client=s->texture_client;
        s->texture_output_frame=q->previous;s->texture_output_offset=q->offset;s->texture_output_bytes=q->bytes;
        atomic_store_explicit(&s->texture_output_state,1,memory_order_release);return 1;
    }
    if(q->operation==PVR_TEXTURE_BEGIN) {
        uint32_t stride,bytes;
        if(q->generation || q->offset || q->bytes || !pvr_render_texture_state_valid(s->owner->draw,q->state) || s->sequence>=PVR_RENDER_DRAW_MAX_FRAMES ||
            s->texture_generation==UINT64_MAX || !idle(s) ||
            !pvr_render_texture_geometry(q->input.width,q->input.height,&stride,&bytes) || q->input.stride!=stride ||
            !q->input.vertex_count || q->input.vertex_count>PVR_RENDER_TEXTURE_VERTICES ||
            q->input.index_count<3 || q->input.index_count>PVR_RENDER_TEXTURE_INDICES || q->input.index_count%3)return 0;
        s->texture_input=q->input;s->texture_render_state=q->state;
        s->texture_total=q->input.vertex_count*24+q->input.index_count*2+pvr_render_batch_table_bytes(s->owner->draw,q->state)+bytes;
        s->texture_received=0;s->texture_previous=q->previous;++s->texture_generation;
        s->texture_draft=1;s->texture_draft_deadline=deadline;return 1;
    }
    if(!s->texture_draft || q->generation!=s->texture_generation || q->previous!=s->texture_previous)return 0;
    if(clock>=s->texture_draft_deadline){s->texture_draft=0;return 0;}
    if(q->operation==PVR_TEXTURE_WRITE) {
        /* Input areas belong to the last frame until both its timelines are
         * acquired; a refused or abandoned upload leaves no frame using them. */
        if(!q->bytes || q->bytes>PVR_RENDER_TEXTURE_CHUNK || q->offset!=s->texture_received ||
            q->offset>s->texture_total || q->bytes>s->texture_total-q->offset || !idle(s))return 0;
        if(!texture_place(s,&l,base,q->offset,q->bytes))return failed(s,PVR_RENDER_SERVICE_MEMORY);
        if(!now(s,&clock) || clock>=deadline || clock>=s->texture_draft_deadline)return 0;
        s->texture_received+=q->bytes;return 1;
    }
    if(q->operation!=PVR_TEXTURE_SUBMIT || q->offset || q->bytes || s->texture_received!=s->texture_total ||
        s->sequence>=PVR_RENDER_DRAW_MAX_FRAMES)return 0;
    /* Validate in place. Only this worker writes the input areas, and only
     * for WRITE of an open upload, which acceptance below closes. */
    uint32_t vertices=s->texture_input.vertex_count*24,indices=s->texture_input.index_count*2;
    uint32_t batches=pvr_render_batch_table_bytes(s->owner->draw,s->texture_render_state);
    const void *vertex=(const void *)(base+l.geometry),*index=(const void *)(base+l.geometry+PVR_RENDER_TEXTURE_VERTEX_BYTES);
    if(!pvr_render_texture_state_valid(s->owner->draw,s->texture_render_state) ||
        (s->owner->draw==PVR_RENDER_BATCH_PROFILE?
         !pvr_render_batch_input_valid(&s->texture_input,s->texture_render_state,vertex,index,s->texture_batches,
            s->texture_total-vertices-indices-batches):
         !pvr_render_texture_input_valid_profile(s->owner->draw,&s->texture_input,vertex,index,
            s->texture_total-vertices-indices)))return 0;
    uint32_t previous=s->sequence;
    if(previous ? (s->phase!=PVR_RENDER_SERVICE_COMPLETE || s->checked!=l.output_bytes ||
        s->pair.geometry!=&s->geometry || s->pair.fragment!=&s->fragment ||
        s->geometry.queue!=&s->owner->service->queue || s->fragment.queue!=&s->owner->service->queue ||
        s->geometry.context!=s || s->fragment.context!=s ||
        s->geometry.firmware_context!=A733_GPU_RENDER_FW || s->fragment.firmware_context!=A733_GPU_RENDER_FW+152 ||
        !pvr_cccb_pair_reusable(&s->pair) || !s->pair.fragment_tail ||
            s->fragment.serial!=(uint64_t)previous*2 || s->geometry.serial!=previous) :
        (s->phase!=PVR_RENDER_SERVICE_READY || s->draw_accepted))return 0;
    if(!idle(s))return 0;
    struct pvr_render_job_image image;uint32_t next=previous+1;
    if(!pvr_render_job_build_frame_pr(&s->programs,s->owner->target.data_firmware[(next-1)&1],
        A733_GPU_RENDER_FW+1408,A733_GPU_RENDER_FW+1412,next,&image))return 0;
    uint64_t scissor=PVR_RENDER_DRAW_SCISSOR;memcpy(image.fragment+112,&scissor,8);
    if(s->owner->draw==3 && !pvr_render_texture_depth_job(s->owner->profile.render.width,
        s->owner->profile.render.height,&image))return 0;
    if(s->owner->draw==PVR_RENDER_BATCH_PROFILE && !pvr_render_batch_depth_job(s->owner->profile.render.width,
        s->owner->profile.render.height,&image))return 0;
    if(!now(s,&clock) || clock>=deadline || clock>=s->texture_draft_deadline)return 0;
    /* Acceptance closes the upload; the worker then writes only the output
     * guards and poison, the command words and the descriptor. */
    s->image=image;s->sequence=next;s->checked=0;s->deadline=deadline;
    s->texture_draft=0;s->texture_cursor=0;s->texture_preparing=1;
    s->phase=PVR_RENDER_SERVICE_TEXTURE_PREPARE;return 1;
}
static int texture_prepare(struct pvr_render_service *s)
{
    struct pvr_render_texture_layout l;
    if(!pvr_render_texture_layout_profile(s->owner->profile.render.width,s->owner->profile.render.height,s->owner->draw,&l))return -1;
    uintptr_t base=s->owner->payload_extent.cpu+s->owner->render.payload_bytes;
    /* Read-back keeps the poison fill until a board run shows full tile
     * coverage. The guards cover the 64 bytes before the frame and the rest
     * of the allocation after it. The GPU clears depth and stencil on chip and
     * stores them without a load, so the CPU never writes the attachment. Before
     * the first frame it writes the two depth guard pages and removes any cache
     * line over the attachment, which no later CPU access recreates. One 64 KiB
     * chunk per step. */
    uint32_t depth=l.depth && s->sequence==1?l.depth_bytes+8192u:0;
    if(s->texture_cursor<l.output_allocation+depth) {
        uint64_t clock;
        if(!now(s,&clock) || clock>=s->deadline)return failed(s,PVR_RENDER_SERVICE_CLOCK);
        /* Offsets are relative to the area; guards are [0, head) and [tail, end). */
        uint32_t first=s->texture_cursor,end=l.output_allocation,head=64,tail=l.output_bytes-64;
        uintptr_t area=base+l.output;int output=first<end;
        if(!output){first-=end;end=depth;area=base+l.depth;head=4096;tail=l.depth_bytes+4096;}
        if(end-first>PVR_RENDER_TEXTURE_CHUNK)end=first+PVR_RENDER_TEXTURE_CHUNK;
        fill(area,first,end<head?end:head,PVR_RENDER_GUARD);
        if(output)fill(area,first>head?first:head,end<tail?end:tail,PVR_RENDER_POISON);
        fill(area,first>tail?first:tail,end,PVR_RENDER_GUARD);
        if(!aarch64_device_cache(AARCH64_CACHE_CLEAN_INVALIDATE,area+first,end-first))return failed(s,PVR_RENDER_SERVICE_MEMORY);
        s->texture_cursor+=end-first;
        return 0;
    }
    uintptr_t commands=base+8192+0xf000,metadata=base+8192+0x10000,batch=base+8192+PVR_RENDER_BATCH_VDM_OFFSET;
    if(s->owner->draw==PVR_RENDER_BATCH_PROFILE) {
        /* The VDM stream and the per-batch PPP blocks share one page. */
        if(!pvr_render_batch_descriptor((void *)(metadata+0x100),&s->texture_input,s->texture_render_state) ||
            !pvr_render_batch_commands((void *)base,s->owner->profile.render.width,s->owner->profile.render.height,
                s->texture_render_state,s->texture_batches,s->texture_input.index_count) ||
            !lines(AARCH64_CACHE_CLEAN,batch,PVR_RENDER_BATCH_PPP_OFFSET-PVR_RENDER_BATCH_VDM_OFFSET+
                (s->texture_render_state&31u)*PVR_RENDER_BATCH_PPP_STRIDE))return failed(s,PVR_RENDER_SERVICE_MEMORY);
    } else {
        if(!pvr_render_texture_descriptor((void *)(metadata+0x100),&s->texture_input))return failed(s,PVR_RENDER_SERVICE_MEMORY);
        memcpy((void *)(base+8192+PVR_RENDER_TEXTURE_COUNT_OFFSET),&s->texture_input.index_count,4);
        if(s->owner->draw==3 && !pvr_render_texture_state_apply((void *)base,s->texture_render_state))
            return failed(s,PVR_RENDER_SERVICE_MEMORY);
        /* The index count and the PPP state words lie in the first 512 bytes. */
        if(!lines(AARCH64_CACHE_CLEAN,commands,512))return failed(s,PVR_RENDER_SERVICE_MEMORY);
    }
    /* The descriptor is 80 bytes; the last maintenance ends with DSB SY. */
    if(!lines(AARCH64_CACHE_CLEAN,metadata+0x100,80))return failed(s,PVR_RENDER_SERVICE_MEMORY);
    s->draw_accepted=1;s->texture_preparing=0;
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
            /* One 64 KiB chunk of the output per step, then both depth guard
             * pages, which are compared here. */
            uintptr_t base=s->owner->payload_extent.cpu+s->owner->render.payload_bytes;
            if(s->checked<l.output_allocation) {
                uint32_t n=l.output_allocation-s->checked;if(n>PVR_RENDER_TEXTURE_CHUNK)n=PVR_RENDER_TEXTURE_CHUNK;
                if(!aarch64_device_cache(AARCH64_CACHE_INVALIDATE,base+l.output+s->checked,n))
                    return failed(s,PVR_RENDER_SERVICE_MEMORY);
                s->checked+=n;
                if(s->checked<l.output_allocation || l.depth)break;
            } else if(l.depth) for(unsigned page=0;page<2;++page) {
                uintptr_t guard=base+l.depth+(page?l.depth_bytes+4096u:0);
                if(!aarch64_device_cache(AARCH64_CACHE_INVALIDATE,guard,4096))return failed(s,PVR_RENDER_SERVICE_MEMORY);
                for(unsigned i=0;i<4096;i+=4)
                    if(*(const payload_word *)(guard+i)!=PVR_RENDER_GUARD)return failed(s,PVR_RENDER_SERVICE_RESULT);
            }
            s->checked=0;s->phase=PVR_RENDER_SERVICE_OUTPUT;
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
            /* Compare only the guards: the 64 bytes before the frame and the
             * rest of the page-rounded allocation after it. The frame itself
             * stays in place for PRESENT and READ. */
            uintptr_t output=s->owner->payload_extent.cpu+s->owner->render.payload_bytes+l.output;
            for(uint32_t at=0;at<l.output_allocation;at+=4) {
                if(at==64)at=l.output_bytes-64;
                if(*(const payload_word *)(output+at)!=PVR_RENDER_GUARD)return failed(s,PVR_RENDER_SERVICE_RESULT);
            }
            s->checked=l.output_bytes;s->phase=PVR_RENDER_SERVICE_COMPLETE;
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
