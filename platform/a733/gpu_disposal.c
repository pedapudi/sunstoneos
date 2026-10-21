#include "a733_gpu_disposal.h"
#include "a733_gpu_graphics_memory.h"
#include "a733_gpu_backend.h"
#include "aarch64_device_memory.h"
#include <string.h>
#ifndef SUNSTONE_GPU_LIFECYCLE
struct a733_gpu_disposal *gpu_application_disposal_get(struct pvr_gpu_service *s)
{ (void)s;return NULL; }
#endif
static int extent_equal(struct device_memory_extent a,struct device_memory_extent b)
{ return a.cpu==b.cpu && a.physical==b.physical && a.bytes==b.bytes; }
static int mapping(struct device_memory_extent e,unsigned attr)
{
    if(!e.bytes || (e.cpu&4095) || (e.bytes&4095) || e.cpu!=e.physical || e.bytes>UINTPTR_MAX-e.cpu)return 0;
    for(uintptr_t p=e.cpu;p<e.cpu+e.bytes;p+=4096) {
        struct aarch64_translation r,w;
        if(!aarch64_device_translate(p,0,&r) || !aarch64_device_translate(p,1,&w) ||
           r.physical!=p || w.physical!=p || r.attributes!=attr || w.attributes!=attr ||
           r.shareability!=w.shareability || (r.shareability!=3 && (attr!=0x44 || r.shareability!=2)))return 0;
    }
    return 1;
}
static int ledger(struct a733_gpu_disposal *d)
{
    struct a733_gpu_reserved *f=d->firmware;
    struct device_memory_lease all[9]={f->wb_lease,f->nc_lease};
    struct device_memory_extent ext[9]={f->wb_extent,f->nc_extent};
    for(unsigned i=0;i<d->count;++i){all[i+2]=d->leases[i];ext[i+2]=d->extents[i];}
    for(unsigned i=0;i<d->count+2;++i) {
        struct device_memory_pool *pool=(void *)all[i].pool;
        struct device_memory_extent actual;enum device_memory_state state;
        if((pool!=f->wb_pool && pool!=f->nc_pool) || !device_memory_describe(pool,all[i],&actual,&state) ||
           state!=DEVICE_MEMORY_DEVICE || !extent_equal(actual,ext[i]))return 0;
    }
    struct device_memory_pool *pools[]={f->wb_pool,f->nc_pool};
    for(unsigned p=0;p<2;++p)for(unsigned slot=0;slot<DEVICE_MEMORY_MAX_ALLOCATIONS;++slot) {
        const struct device_memory_allocation *a=&pools[p]->allocations[slot];
        if(a->state==DEVICE_MEMORY_FREE)continue;
        unsigned found=0;
        for(unsigned i=0;i<d->count+2;++i)if(all[i].pool==(uintptr_t)pools[p] && all[i].slot==slot &&
            all[i].owner==a->owner && all[i].generation==a->generation)++found;
        if(found!=1)return 0;
    }
    return 1;
}
static int fail(struct a733_gpu_disposal *d,unsigned error)
{
    if(d->phase==A733_GPU_DISPOSAL_FAILED)return -1;
    d->error=error;d->phase=A733_GPU_DISPOSAL_FAILED;
    for(unsigned i=0;i<d->count;++i)if(!(d->released&(1u<<i)))
        (void)device_memory_retain((void *)d->leases[i].pool,d->leases[i]);
    return -1;
}
static int timely(struct pvr_gpu_service *s,struct a733_gpu_disposal *d)
{
    uint64_t now;
    if(!s->config.start.now(s->config.context,&now) || now<s->last_now || now>=s->deadline || now>=d->limit)return 0;
    s->last_now=now;return 1;
}
int a733_gpu_disposal_begin(struct pvr_gpu_service *s,uint64_t epoch)
{
    struct a733_gpu_disposal *d=gpu_application_disposal_get(s);
    if(!d || (d->phase!=A733_GPU_DISPOSAL_NEW && d->phase!=A733_GPU_DISPOSAL_LIVE) ||
       s->phase!=PVR_GPU_SERVICE_STOPPED || !epoch || epoch!=s->epoch || epoch!=s->closed_epoch ||
       s->closed_owner>=PVR_GPU_SERVICE_CLIENTS || !a733_gpu_backend_stopped(d->backend,s) ||
       d->firmware->memory.phase!=A733_GPU_MEMORY_OFFLINE || d->backend->memory!=&d->firmware->memory ||
       s->config.compute!=&d->compute->compute ||
       s->config.graphics!=d->graphics ||
       !a733_gpu_compute_reserved_admitted(d->compute,d->firmware,s))return 0;
    struct pvr_compute_service *c=&d->compute->compute;
    if(c->phase!=PVR_COMPUTE_SERVICE_CLEANED || c->cleanup_status!=1 ||
       c->cleanup_token.epoch!=epoch || !c->cleanup_token.serial ||
       atomic_load_explicit(&c->input_state,memory_order_acquire))return 0;
    if(d->graphics) {
        uint32_t addresses[6],types[6];unsigned count;
        if(!a733_gpu_graphics_memory_cleanup_records(d->graphics,addresses,types,&count) ||
           count!=s->graphics_cleanup_count || memcmp(addresses,s->graphics_cleanup_address,count*4u) ||
           memcmp(types,s->graphics_cleanup_type,count*4u))return 0;
        if(!s->graphics_events_retired || s->graphics_cleanup_count!=(s->config.render?6u:4u) ||
           s->graphics_cleanup_index!=s->graphics_cleanup_count || s->free_lists.request_count ||
           s->free_lists.phase!=PVR_FREE_LIST_IDLE)return 0;
        for(unsigned i=0;i<s->graphics_cleanup_count;++i)if(s->graphics_cleanup_status[i]!=1 ||
            s->graphics_cleanup_tokens[i].epoch!=epoch || !s->graphics_cleanup_tokens[i].serial)return 0;
        if(s->config.render && (s->config.render->phase!=PVR_RENDER_SERVICE_CLEANED ||
           atomic_load_explicit(&s->config.render->draw_state,memory_order_acquire) ||
           atomic_load_explicit(&s->config.render->texture_state,memory_order_acquire) ||
           atomic_load_explicit(&s->config.render->texture_output_state,memory_order_acquire) ||
           s->config.render->texture_draft || s->config.render->texture_preparing))return 0;
    }
    d->leases[0]=d->compute->payload_lease;d->extents[0]=d->compute->payload_extent;d->attributes[0]=0xff;
    d->leases[1]=d->compute->firmware_lease;d->extents[1]=d->compute->firmware_extent;d->attributes[1]=0x44;
    d->leases[2]=d->compute->table_lease;d->extents[2]=d->compute->table_extent;d->attributes[2]=0xff;d->count=3;
    if(d->graphics) {
        struct a733_gpu_graphics_memory *g=d->graphics;
        const struct device_memory_lease leases[]={g->backing_lease,g->payload_lease,g->shared_lease,g->table_lease};
        const struct device_memory_extent extents[]={g->backing_extent,g->payload_extent,g->shared_extent,g->table_extent};
        for(unsigned i=0;i<4;++i){d->leases[3+i]=leases[i];d->extents[3+i]=extents[i];d->attributes[3+i]=i==2?0x44:0xff;}
        d->count=7;
    }
    if(!ledger(d))return 0;
    for(unsigned i=0;i<d->count;++i)if(!mapping(d->extents[i],d->attributes[i]))return 0;
    if(s->deadline>d->limit)s->deadline=d->limit;
    if(!timely(s,d))return 0;
    d->package=d->compute->package;d->compute_owner=(uint32_t)d->compute->binding.owner;
    if(d->graphics){d->profile=d->graphics->profile;d->graphics_owner=(uint32_t)d->graphics->owner;
        d->fixed_clear=d->graphics->fixed_clear;d->draw=d->graphics->draw;}
    unsigned receipt=0,contexts=s->config.render?2:0;
    for(unsigned i=0;i<contexts;++i){d->cleanup_tokens[receipt]=s->graphics_cleanup_tokens[i];d->cleanup_status[receipt++]=s->graphics_cleanup_status[i];}
    d->cleanup_tokens[receipt]=c->cleanup_token;d->cleanup_status[receipt++]=c->cleanup_status;
    if(d->graphics)for(unsigned i=contexts;i<s->graphics_cleanup_count;++i){d->cleanup_tokens[receipt]=s->graphics_cleanup_tokens[i];d->cleanup_status[receipt++]=s->graphics_cleanup_status[i];}
    d->epoch=epoch;d->released=d->transitioned=d->index=d->page=d->error=0;d->phase=A733_GPU_DISPOSAL_CACHE;return 1;
}
int a733_gpu_disposal_poll(struct pvr_gpu_service *s)
{
    struct a733_gpu_disposal *d=gpu_application_disposal_get(s);
    if(!d || d->phase==A733_GPU_DISPOSAL_FAILED)return -1;
    if(s->phase!=PVR_GPU_SERVICE_DISPOSING || s->epoch!=d->epoch || !a733_gpu_backend_stopped(d->backend,s) ||
       !timely(s,d))return fail(d,1);
    if(d->phase==A733_GPU_DISPOSAL_CACHE) {
        if(d->index==d->count){d->phase=A733_GPU_DISPOSAL_UNMAP;d->index=d->page=0;return 0;}
        struct device_memory_extent e=d->extents[d->index];
        if(d->attributes[d->index]==0xff)for(unsigned n=0;n<16 && (size_t)d->page*4096<e.bytes;++n,++d->page)
            if(!aarch64_device_cache(AARCH64_CACHE_INVALIDATE,e.cpu+(size_t)d->page*4096,4096))return fail(d,2);
        if(d->attributes[d->index]!=0xff || (size_t)d->page*4096==e.bytes){++d->index;d->page=0;}return 0;
    }
    if(d->phase==A733_GPU_DISPOSAL_UNMAP) {
        struct pvr_mips_object objects[8];unsigned count=6;
        memcpy(objects,d->compute->compute.objects,6*sizeof(*objects));
        if(d->graphics){objects[count++]=d->graphics->object;if(s->config.render)objects[count++]=d->graphics->render_object;}
        const struct pvr_mips_buffer *table=&d->firmware->config.firmware_buffers[2];
        if(!pvr_mips_unmap_stopped(table,objects,count) ||
           !aarch64_device_cache(AARCH64_CACHE_CLEAN,(uintptr_t)table->storage.cpu,table->storage.bytes))return fail(d,3);
        memset(s->mappings+PVR_FW_RESOURCE_COUNT,0,(PVR_MIPS_MAX_OBJECTS-PVR_FW_RESOURCE_COUNT)*sizeof(*s->mappings));
        d->phase=A733_GPU_DISPOSAL_TABLES;return 0;
    }
    if(d->phase==A733_GPU_DISPOSAL_TABLES) {
        struct a733_gpu_vm_memory *vm=d->index?&d->graphics->vm:&d->compute->vm;
        if(vm->phase!=A733_GPU_VM_MEMORY_PUBLISHED)return fail(d,4);
        for(unsigned n=0;n<16 && d->page<vm->tables.page_count;++n,++d->page) {
            uintptr_t at=(uintptr_t)vm->tables.cpu+(size_t)d->page*4096;
            memset((void *)at,0,4096);
            if(!aarch64_device_cache(AARCH64_CACHE_CLEAN,at,4096))return fail(d,4);
        }
        if(d->page==vm->tables.page_count) {
            memset(vm->context_cpu,0,PVR_GPU_VM_CONTEXT_BYTES);__asm__ volatile("dsb sy":::"memory");
            vm->phase=A733_GPU_VM_MEMORY_OFFLINE;d->page=0;
            if(d->index || !d->graphics){d->phase=A733_GPU_DISPOSAL_RELEASE;d->index=0;}else d->index=1;
        }
        return 0;
    }
    if(d->phase==A733_GPU_DISPOSAL_RELEASE) {
        if(d->index<d->count) {
            unsigned i=d->index;struct device_memory_pool *pool=(void *)d->leases[i].pool;
            struct device_memory_allocation *a=device_memory_lookup(pool,d->leases[i]);
            struct device_memory_extent e;
            if(!a || a->state!=DEVICE_MEMORY_DEVICE || !device_memory_describe(pool,d->leases[i],&e,NULL) ||
               !extent_equal(e,d->extents[i]))return fail(d,5);
            a->state=DEVICE_MEMORY_CPU;d->transitioned|=1u<<i;
            if(!device_memory_release(pool,d->leases[i]))return fail(d,5);
            d->released|=1u<<i;++d->index;return 0;
        }
        if(!a733_gpu_compute_reserved_disposed(d->compute) ||
           (d->graphics && !a733_gpu_graphics_memory_disposed(d->graphics)))return fail(d,6);
        d->phase=A733_GPU_DISPOSAL_DONE;return 1;
    }
    return fail(d,7);
}
int a733_gpu_disposal_rebuild(struct pvr_gpu_service *s)
{
    struct a733_gpu_disposal *d=gpu_application_disposal_get(s);
    if(!d || d->phase!=A733_GPU_DISPOSAL_DONE || d->epoch!=s->closed_epoch ||
       d->released!=((1u<<d->count)-1) || !a733_gpu_backend_stopped(d->backend,s) || !timely(s,d))return 0;
    d->phase=A733_GPU_DISPOSAL_REBUILD;
    /* Only successfully disposed child metadata may be replaced. Global service,
     * backend, client tickets, providers and the disposal receipt stay intact. */
    if(!a733_gpu_compute_reserved_rebuild(d->compute,s) ||
       (d->graphics && !a733_gpu_graphics_memory_rebuild(d->graphics,s)))return fail(d,8);
    s->config=d->firmware->config;
    if(!a733_gpu_backend_rebind_children(d->backend) || !timely(s,d))return fail(d,8);
    d->firmware->config=s->config;
    d->phase=A733_GPU_DISPOSAL_LIVE;return 1;
}
