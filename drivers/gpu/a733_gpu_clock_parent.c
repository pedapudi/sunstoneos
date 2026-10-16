#include "a733_gpu_clock_parent.h"
#include <stddef.h>

static int separate(const void *a,size_t an,const void *b,size_t bn)
{
    uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
    return a && b && an<=UINTPTR_MAX-x && bn<=UINTPTR_MAX-y && (x+an<=y || y+bn<=x);
}
int a733_gpu_clock_parent_init(struct a733_gpu_clock_parent *s,struct a733_clock_leases *m,
    struct a733_gpu_resource_manager *r,unsigned client,uint64_t owner,uint64_t deadline)
{
    if(!separate(s,sizeof(*s),m,sizeof(*m)) || !separate(s,sizeof(*s),r,sizeof(*r)) ||
        !separate(m,sizeof(*m),r,sizeof(*r)) || s->phase!=A733_GPU_CLOCK_NEW ||
        !m->initialized || m->failed || !r->initialized || r->failed ||
        client>=A733_GPU_RESOURCE_CLIENTS || !owner || !deadline)return 0;
    for(unsigned i=0;i<A733_GPU_RESOURCE_SLOTS;++i)
        if(r->leases[i].mode!=A733_GPU_RESOURCE_FREE)return 0;
    *s=(struct a733_gpu_clock_parent){.registry=m,.resources=r,.owner=owner,
        .deadline=deadline,.client=client,.phase=A733_GPU_CLOCK_COMMON};
    return 1;
}
static int failed(struct a733_gpu_clock_parent *s)
{s->phase=A733_GPU_CLOCK_FAILED;return -1;}
static int time_ok(struct a733_gpu_clock_parent *s)
{
    uint64_t now;
    if(s->resources->ops.now(s->resources->context,&now)!=1 || now<s->last_now ||
        now<s->resources->last_now || now>=s->deadline)return 0;
    s->last_now=now;return 1;
}
int a733_gpu_clock_parent_poll(struct a733_gpu_clock_parent *s)
{
    if(!s || s->phase==A733_GPU_CLOCK_NEW)return -1;
    if(s->phase==A733_GPU_CLOCK_FAILED)return -1;
    if(s->phase==A733_GPU_CLOCK_READY)return 1;
    if(!time_ok(s))return failed(s);
    if(s->phase==A733_GPU_CLOCK_COMMON) {
        enum a733_clock_result r=a733_clock_acquire(s->registry,s->owner,A733_CLOCK_PERI0,0,s->deadline,&s->common);
        if(r==A733_CLOCK_OK)s->phase=A733_GPU_CLOCK_LEGACY;
        else if(r!=A733_CLOCK_BUSY)return failed(s);
    } else if(s->phase==A733_GPU_CLOCK_LEGACY) {
        struct a733_gpu_power_resources ready;
        enum a733_gpu_resource_result r=a733_gpu_resources_acquire(s->resources,s->client,
            A733_GPU_RESOURCE_PARENT,s->deadline,&s->token,&ready);
        if(r==A733_GPU_RESOURCE_OK) {
            if(ready.parent_hz!=A733_GPU_INITIAL_HZ)return failed(s);
            s->phase=A733_GPU_CLOCK_READY;
        } else if(r!=A733_GPU_RESOURCE_BUSY)return failed(s);
    } else return failed(s);
    if(!time_ok(s))return failed(s);
    return s->phase==A733_GPU_CLOCK_READY ? 1 : 0;
}
