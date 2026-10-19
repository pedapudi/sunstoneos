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
        s->owner->service->phase==PVR_GPU_SERVICE_READY &&
        s->owner->service->queue.phase==PVR_CCB_ACTIVE &&
        s->owner->service->epoch==s->epoch;
}
static int now(void *context,uint64_t *out)
{
    struct pvr_render_service *s=context;
    if (!admitted(s)) return 0;
    struct pvr_gpu_service *g=s->owner->service;
    if(g->config.start.now(g->config.context,out)!=1 || *out<s->last_now ||
        *out<g->last_now || *out<g->start.last_now || *out<g->control.last_now ||
        *out<g->queue.last_now || *out<g->free_lists.last_now ||
        *out<s->geometry.last_now || *out<s->fragment.last_now ||
        *out<s->pair.last_now || (g->config.compute && *out<g->config.compute->last_now)) return 0;
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
    if(!now(s,&clock) || clock>=deadline){failed(s,PVR_RENDER_SERVICE_CLOCK);return 0;}
    return 1;
}
int pvr_render_service_draw(struct pvr_render_service *s,const void *vertices,size_t bytes,uint32_t count,uint64_t deadline)
{
    if(!s || !s->owner || s->owner->draw!=1 || s->phase!=PVR_RENDER_SERVICE_READY ||
        s->draw_accepted || !pvr_render_draw_vertices_valid(vertices,bytes,count))return 0;
    s->epoch=s->owner->service->epoch;
    uint64_t clock;
    if(!admitted(s) || !now(s,&clock) || deadline<=clock ||
        pvr_compute_service_admit_mode(s->owner->service->config.compute)!=1)return 0;
    uintptr_t timelines=a733_gpu_graphics_memory_render_shared(s->owner)+1408;
    if(!mapped(timelines,8,0x44) || __atomic_load_n((uint32_t *)timelines,__ATOMIC_ACQUIRE) ||
        __atomic_load_n((uint32_t *)(timelines+4),__ATOMIC_ACQUIRE))return 0;
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
    __asm__ volatile("dsb sy":::"memory");
    s->draw_accepted=1;
    if(!pvr_render_service_begin(s,deadline))return failed(s,PVR_RENDER_SERVICE_ADMISSION);
    return 1;
}
int pvr_render_service_poll(struct pvr_render_service *s)
{
    if(!s || s->phase==PVR_RENDER_SERVICE_FAILED)return -1;
    if(s->phase==PVR_RENDER_SERVICE_COMPLETE)return 1;
    if(s->phase<PVR_RENDER_SERVICE_FORMAT_GEOMETRY || !admitted(s))return failed(s,PVR_RENDER_SERVICE_ADMISSION);
    uint64_t clock;
    if(!now(s,&clock) || clock>=s->deadline)return failed(s,PVR_RENDER_SERVICE_CLOCK);
    switch(s->phase) {
    case PVR_RENDER_SERVICE_FORMAT_GEOMETRY:
    case PVR_RENDER_SERVICE_FORMAT_FRAGMENT: {
        struct pvr_cccb *client=s->phase==PVR_RENDER_SERVICE_FORMAT_GEOMETRY?&s->geometry:&s->fragment;
        enum pvr_cccb_result result=pvr_cccb_poll(client);
        if(result==PVR_CCCB_FAULT || result==PVR_CCCB_INVALID)return failed(s,PVR_RENDER_SERVICE_QUEUE);
        if(result==PVR_CCCB_OK)++s->phase;
        break;
    }
    case PVR_RENDER_SERVICE_SUBMIT:
        if(!s->pair.phase) {
            struct pvr_cccb_command geom[]={
                {PVR_CCB_MAGIC|PVR_CCCB_TASK|201,112,1,1,s->image.geometry},
                {PVR_CCB_MAGIC|213,8,0,0,s->image.geometry_update}};
            struct pvr_cccb_command frag[]={
                {PVR_CCB_MAGIC|215,8,0,0,s->image.fragment_fence},
                {PVR_CCB_MAGIC|PVR_CCCB_TASK|203,480,1,1,s->image.fragment},
                {PVR_CCB_MAGIC|213,8,0,0,s->image.fragment_update}};
            uint32_t cleanup=s->owner->target.data_firmware[0]+320;
            if(!pvr_cccb_pair_begin(&s->pair,&s->geometry,&s->fragment,geom,2,frag,3,&cleanup,1,&cleanup,1,s->deadline))
                return failed(s,PVR_RENDER_SERVICE_QUEUE);
        } else {
            int result=pvr_cccb_pair_poll(&s->pair);
            if(result<0)return failed(s,PVR_RENDER_SERVICE_QUEUE);
            if(result==1)s->phase=PVR_RENDER_SERVICE_TIMELINES;
        }
        break;
    case PVR_RENDER_SERVICE_TIMELINES: {
        uintptr_t shared=a733_gpu_graphics_memory_render_shared(s->owner)+1408;
        if(!mapped(shared,8,0x44))return failed(s,PVR_RENDER_SERVICE_MEMORY);
        /* Read the dependent fragment first: geometry may complete between loads.
         * The acquire orders the subsequent prerequisite timeline read. */
        uint32_t b=__atomic_load_n((uint32_t *)(shared+4),__ATOMIC_ACQUIRE);
        uint32_t a=__atomic_load_n((uint32_t *)shared,__ATOMIC_ACQUIRE);
        if(a>1 || b>1 || (b && !a))return failed(s,PVR_RENDER_SERVICE_TIMELINE);
        if(a==1 && b==1){s->phase=PVR_RENDER_SERVICE_INVALIDATE;s->checked=0;}
        break;
    }
    case PVR_RENDER_SERVICE_INVALIDATE: {
        uintptr_t output=s->owner->payload_extent.cpu+s->owner->render.payload_bytes+8192+PVR_RENDER_OUTPUT_OFFSET;
        uintptr_t p=output+s->checked*4096u;
        if(!mapped(p,4096,0xff) || !aarch64_device_cache(AARCH64_CACHE_INVALIDATE,p,4096))return failed(s,PVR_RENDER_SERVICE_MEMORY);
        if(++s->checked==5){s->checked=0;s->phase=PVR_RENDER_SERVICE_OUTPUT;}
        break;
    }
    case PVR_RENDER_SERVICE_OUTPUT: {
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
