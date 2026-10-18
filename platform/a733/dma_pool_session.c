#include "a733_dma_pool_session.h"
#include "aarch64_device_memory.h"
#include "a733_i2s_platform.h"

_Static_assert(A733_DMA_CHANNELS<=DMA_SERVICE_JOBS,"one job per selected channel");

static struct a733_dma_pool_session *instance;
static struct device_memory_pool *owned_pool;
static struct device_memory_lease owned_lease;
static struct device_memory_extent owned_extent, pool_extent;
static struct device_memory_allocation owned_record;
static unsigned creating_bootstrap;
static void bootstrap(void *);

static int privileged(void)
{ uint64_t v; __asm__ volatile("mrs %0,CurrentEL":"=r"(v)); return v==4; }
static int separate(uintptr_t a,size_t an,uintptr_t b,size_t bn)
{ return a && b && an && bn && an<=UINTPTR_MAX-a && bn<=UINTPTR_MAX-b &&
    (a+an<=b || b+bn<=a); }
static int image(const void *p,size_t bytes,int write)
{
    uintptr_t start=(uintptr_t)p;
    if(start<0x41000000 || start>=0x42000000 || !bytes || bytes>0x42000000-start ||
       !separate(start,bytes,pool_extent.cpu,pool_extent.bytes)) return 0;
    uintptr_t end=start+bytes;
    for(uintptr_t a=start&~(uintptr_t)4095;a<end;a+=4096) {
        struct aarch64_translation t;
        if(!aarch64_device_translate(a,write,&t) || t.physical!=a ||
           t.attributes!=0xff || t.shareability!=3) return 0;
    }
    return 1;
}
static int same_extent(struct device_memory_extent a,struct device_memory_extent b)
{ return a.cpu==b.cpu && a.physical==b.physical && a.bytes==b.bytes; }
static int parent(const struct a733_dma_pool_session *s)
{
    if(!s || s!=instance || !s->admitted || s->pool!=owned_pool ||
       s->lease.pool!=owned_lease.pool || s->lease.slot!=owned_lease.slot ||
       s->lease.owner!=owned_lease.owner || s->lease.generation!=owned_lease.generation ||
       !same_extent(s->extent,owned_extent) || !owned_pool || !owned_pool->initialized ||
       !same_extent(owned_pool->backing,pool_extent) ||
       owned_lease.slot>=DEVICE_MEMORY_MAX_ALLOCATIONS) return 0;
    /* This exact permanent slot has no writers after boot. Other consumers may
     * mutate their slots under the common pool serialization contract. */
    const struct device_memory_allocation *r=&owned_pool->allocations[owned_lease.slot];
    return r->owner==owned_record.owner && r->generation==owned_record.generation &&
        r->first==owned_record.first && r->pages==owned_record.pages &&
        r->state==DEVICE_MEMORY_DEVICE && s->count && s->count<=DMA_SERVICE_JOBS &&
        owned_extent.bytes==(size_t)(s->periods?s->periods+1:s->count*3)*DEVICE_MEMORY_PAGE &&
        (!s->periods || (s->count==1 && s->periods>=2 && s->periods<=A733_I2S_STREAM_PERIODS));
}
static int exact_mapping(struct a733_dma_pool_session *s,const struct a733_dma_mapping *m)
{
    if(!parent(s) || !m || (unsigned)m->access>A733_DMA_DESCRIPTORS ||
       (uintptr_t)m->cpu<owned_extent.cpu) return 0;
    size_t off=(uintptr_t)m->cpu-owned_extent.cpu;
    if(s->periods){
        if(off>=owned_extent.bytes || m->bus!=owned_extent.physical+off)return 0;
        if(m->access==A733_DMA_DESCRIPTORS){
            if(!((off==0 && m->bytes==4096) ||
                 (off%64==0 && off/64<s->periods && m->bytes==64)))return 0;
        }else if(off<4096 || off%4096 || m->bytes!=4096 ||
                 m->access!=(s->direction==A733_I2S_RECEIVE?A733_DMA_WRITE:A733_DMA_READ))return 0;
        return a733_dma_native_operations.validate_mapping(&s->native,m);
    }
    if(m->bytes!=DEVICE_MEMORY_PAGE || off>=owned_extent.bytes || off%DEVICE_MEMORY_PAGE ||
       m->bus!=owned_extent.physical+off) return 0;
    unsigned bank=(unsigned)(off/DEVICE_MEMORY_PAGE)/s->count;
    enum a733_dma_access role=bank==0 ? A733_DMA_DESCRIPTORS : bank==1 ? A733_DMA_READ : A733_DMA_WRITE;
    return m->access==role && a733_dma_native_operations.validate_mapping(&s->native,m);
}
static _Noreturn void fail(struct a733_dma_pool_session *s)
{
    if(s==instance && s) {
        atomic_store_explicit(&s->state,A733_DMA_POOL_FAILED,memory_order_release);
        if(s->native.fatal) s->native.fatal(s->native.fatal_cookie);
    }
    __builtin_trap();
}
static void worker_owner(struct a733_dma_pool_session *s)
{
    if(!privileged() || !parent(s) || xTaskGetSchedulerState()!=taskSCHEDULER_RUNNING ||
       xTaskGetCurrentTaskHandle()!=s->service.worker || !a733_dma_startup_ready(&s->startup) ||
       atomic_load_explicit(&s->startup.clock->failed,memory_order_acquire) ||
       atomic_load_explicit(&s->startup.registry->failed,memory_order_acquire) ||
       atomic_load_explicit(&s->startup.locks->retained,memory_order_acquire)) fail(s);
}
static uint32_t read_reg(void *cookie,uintptr_t address)
{ struct a733_dma_pool_session *s=cookie;worker_owner(s);return a733_dma_native_operations.read(&s->native,address); }
static void write_reg(void *cookie,uintptr_t address,uint32_t value)
{ struct a733_dma_pool_session *s=cookie;worker_owner(s);a733_dma_native_operations.write(&s->native,address,value); }
static void cache(void *cookie,enum a733_dma_cache operation,void *cpu,size_t bytes)
{
    struct a733_dma_pool_session *s=cookie;worker_owner(s);
    if((uintptr_t)cpu<owned_extent.cpu) fail(s);
    size_t off=(uintptr_t)cpu-owned_extent.cpu;
    int page=bytes==4096 && off%4096==0;
    int descriptor=s->periods && bytes==64 && off%64==0 && off/64<s->periods;
    if(off>=owned_extent.bytes || (!page&&!descriptor) || bytes>owned_extent.bytes-off) fail(s);
    a733_dma_native_operations.cache(&s->native,operation,cpu,bytes);
}
static void barrier(void *cookie)
{ struct a733_dma_pool_session *s=cookie;worker_owner(s);a733_dma_native_operations.barrier(&s->native); }
static uint64_t ticks(void *cookie)
{ struct a733_dma_pool_session *s=cookie;worker_owner(s);return a733_dma_native_operations.ticks(&s->native); }
static int mapping(void *cookie,const struct a733_dma_mapping *m)
{
    if(!privileged()) return 0;
    if(xTaskGetSchedulerState()==taskSCHEDULER_RUNNING) worker_owner(cookie);
    return exact_mapping(cookie,m);
}
static int fifo(void *cookie,const struct a733_dma_endpoint *endpoint,enum a733_dma_access access)
{
    struct a733_dma_pool_session *s=cookie;
    if(!privileged() || !parent(s) || !s->periods)return 0;
    if(xTaskGetSchedulerState()==taskSCHEDULER_RUNNING)worker_owner(s);
    return a733_dma_native_operations.validate_fifo(&s->native,endpoint,access);
}
static const struct a733_dma_operations operations={read_reg,write_reg,cache,barrier,ticks,mapping,fifo};

BaseType_t a733_dma_pool_task_privileged(TaskFunction_t entry,void *argument)
{
    return (creating_bootstrap && entry==bootstrap && argument==instance) ||
        dma_service_task_privileged(entry,argument) ? pdTRUE : pdFALSE;
}
static int prepared(void *cookie,uintptr_t controller,uint16_t channels)
{
    struct a733_dma_pool_session *s=cookie;
    if(!parent(s)) return -1;
    return a733_dma_startup_prepared(&s->startup,controller,channels);
}
static void bootstrap(void *argument)
{
    struct a733_dma_pool_session *s=argument;
    if(s==instance && atomic_load_explicit(&s->state,memory_order_acquire)==A733_DMA_POOL_WAITING) {
        for(;;) {
            uint64_t now;
            if(!parent(s) || !a733_counter_clock_now(s->startup.clock,&now) ||
               now>=s->startup.handoff.allowed.deadline_us) break;
            enum a733_dma_platform_result r=a733_dma_startup_poll(&s->startup);
            if(r==A733_DMA_PLATFORM_RETAINED) break;
            if(r==A733_DMA_PLATFORM_READY) {
                enum dma_service_result a=dma_service_activate(&s->service);
                if(a==DMA_SERVICE_OK) {
                    atomic_store_explicit(&s->state,A733_DMA_POOL_READY,memory_order_release);
                    vTaskSuspend(NULL);fail(s);
                }
                if(a!=DMA_SERVICE_BUSY) break;
            }
            vTaskDelay(1);
        }
        /* Runtime failure keeps the immutable allocation DEVICE. No concurrent
         * pool mutation or release occurs on this fault path. */
        atomic_store_explicit(&s->state,A733_DMA_POOL_FAILED,memory_order_release);
    }
    vTaskSuspend(NULL);fail(s);
}
static int initialize(struct a733_dma_pool_session *s,const struct a733_dma_pool_config *c,
    const struct dma_service_i2s_stream_profile *stream,unsigned periods,unsigned period_bytes,unsigned supervised)
{
    if(!privileged() || xTaskGetSchedulerState()!=taskSCHEDULER_NOT_STARTED || !s || !c ||
       instance || (uintptr_t)s%_Alignof(struct a733_dma_pool_session) ||
       (uintptr_t)c%_Alignof(struct a733_dma_pool_config) ||
       !separate((uintptr_t)s,sizeof(*s),(uintptr_t)c,sizeof(*c))) return 0;
    struct device_memory_pool *pool=a733_memory_reservations_init();
    /* This constructor uses the initial in-image pool profile. A later external
     * carveout requires explicit image-exclusion integration, not subtraction
     * of an arbitrary pool address from the image boundary below. */
    if(!pool || pool->backing.cpu<0x41000000 || pool->backing.cpu>=0x42000000 ||
       pool->backing.physical!=pool->backing.cpu ||
       pool->backing.bytes>=0x42000000-pool->backing.cpu) return 0;
    pool_extent=pool->backing;
    if(!image(s,sizeof(*s),1) || !image(c,sizeof(*c),0) || s->admitted || s->count ||
       s->startup.initialized || s->native.initialized || s->service.initialized ||
       !c->handoff || !c->clock || !c->registry || !c->locks || !c->fatal ||
       !c->drain_ticks || c->drain_ticks>INT64_MAX || !c->copy_bytes || c->copy_bytes>4096 ||
       c->copy_bytes%4 || !c->priority || c->priority>=configMAX_PRIORITIES ||
       !c->affinity || (c->affinity>>configNUMBER_OF_CORES)) return 0;
    const void *objects[]={s,c,c->handoff,c->clock,c->registry,c->locks,pool};
    const size_t sizes[]={sizeof(*s),sizeof(*c),sizeof(*c->handoff),sizeof(*c->clock),
        sizeof(*c->registry),sizeof(*c->locks),sizeof(*pool)};
    const size_t alignments[]={_Alignof(struct a733_dma_pool_session),_Alignof(struct a733_dma_pool_config),
        _Alignof(struct a733_dma_handoff),_Alignof(struct a733_counter_clock),
        _Alignof(struct a733_clock_leases),_Alignof(struct a733_platform_locks),_Alignof(struct device_memory_pool)};
    for(unsigned i=0;i<sizeof(objects)/sizeof(objects[0]);++i) {
        if((uintptr_t)objects[i]%alignments[i] || !image(objects[i],sizes[i],i!=1 && i!=2)) return 0;
        for(unsigned j=0;j<i;++j)
            if(!separate((uintptr_t)objects[i],sizes[i],(uintptr_t)objects[j],sizes[j])) return 0;
    }
    if(!image((const void *)(uintptr_t)c->fatal,4,0) ||
       (c->fatal_cookie && !image(c->fatal_cookie,1,1)) ||
       !(supervised?a733_dma_startup_supervised_init(&s->startup,c->handoff,c->clock,c->registry,c->locks):
         a733_dma_startup_init(&s->startup,c->handoff,c->clock,c->registry,c->locks))) return 0;
    if(stream){
        struct a733_i2s_plan plan;
        uint16_t channels=c->handoff->service_channels;
        if(!image(stream,sizeof(*stream),0) || !stream->stream.cookie ||
           !image(stream->stream.cookie,sizeof(struct a733_i2s_platform),1) ||
           periods<2 || periods>A733_I2S_STREAM_PERIODS || !period_bytes || period_bytes>4096 ||
           !channels || (channels&(channels-1)) ||
           !a733_i2s_plan(&stream->stream.format,&plan) || period_bytes%plan.frame_bytes ||
           period_bytes%(plan.dma_width*8) ||
           (unsigned)stream->stream.direction>A733_I2S_TRANSMIT)return 0;
        s->periods=periods;s->period_bytes=period_bytes;s->direction=stream->stream.direction;
    }
    for(unsigned ch=0;ch<A733_DMA_CHANNELS;++ch)
        if(c->handoff->service_channels&(1u<<ch)) ++s->count;
    s->copy_bytes=c->copy_bytes;
    if(!device_memory_acquire(pool,c->handoff->allowed.owner,(size_t)(periods?periods+1:s->count*3)*4096,
                              &s->lease,&s->extent)) return 0;
    instance=s;owned_pool=pool;owned_lease=s->lease;owned_extent=s->extent;
    s->pool=pool;s->admitted=1;
    struct device_memory_allocation *record=device_memory_lookup(pool,s->lease);
    record->state=DEVICE_MEMORY_DEVICE;owned_record=*record;
    if(stream){
        enum a733_dma_access role=s->direction==A733_I2S_RECEIVE?A733_DMA_WRITE:A733_DMA_READ;
        unsigned width=stream->stream.format.container_bits/8;
        uintptr_t port=s->direction==A733_I2S_RECEIVE?A733_I2S0_RXFIFO:A733_I2S0_TXFIFO;
        struct dma_memory_window windows[]={
            {s->extent.cpu,s->extent.physical,4096,DMA_MEMORY_ROLE(A733_DMA_DESCRIPTORS)},
            {s->extent.cpu+4096,s->extent.physical+4096,(size_t)periods*4096,DMA_MEMORY_ROLE(role)}};
        struct dma_fifo_port fifo={port,A733_I2S0_DMA_REQUEST,1u<<width,1u<<8,
            s->direction==A733_I2S_RECEIVE?A733_DMA_READ:A733_DMA_WRITE};
        if(!a733_dma_native_init(&s->native,c->handoff->controller,c->handoff->controller,
            windows,2,&fifo,1,c->fatal,c->fatal_cookie))goto failed;
        unsigned channel=0;while(!(c->handoff->service_channels&(1u<<channel)))++channel;
        for(unsigned i=0;i<periods;++i){
            uintptr_t cpu=s->extent.cpu+(i+1)*4096;
            uint64_t physical=s->extent.physical+(i+1)*4096;
            s->stream_mappings[i]=(struct a733_dma_mapping){(void *)cpu,physical,4096,role};
            struct a733_dma_endpoint ram={physical,0,width,8},device={port,3,width,8};
            s->segments[i]=(struct a733_dma_segment){s->direction==A733_I2S_RECEIVE?device:ram,
                s->direction==A733_I2S_RECEIVE?ram:device,period_bytes};
            if(!exact_mapping(s,&s->stream_mappings[i]))goto failed;
        }
        s->jobs[0]=(struct dma_service_job){.kind=DMA_SERVICE_I2S_STREAM,.channel=channel,.stream=stream,
            .transfer={{(void *)s->extent.cpu,s->extent.physical,4096,A733_DMA_DESCRIPTORS},
                       s->segments,periods,s->stream_mappings,periods}};
        if(!exact_mapping(s,&s->jobs[0].transfer.descriptors) ||
           !a733_i2s_stream_valid(&stream->stream,&s->jobs[0].transfer))goto failed;
    }else{
    size_t bank=(size_t)s->count*4096;
    struct dma_memory_window windows[3];
    const enum a733_dma_access roles[]={A733_DMA_DESCRIPTORS,A733_DMA_READ,A733_DMA_WRITE};
    for(unsigned i=0;i<3;++i)
        windows[i]=(struct dma_memory_window){s->extent.cpu+i*bank,s->extent.physical+i*bank,bank,
            DMA_MEMORY_ROLE(roles[i])};
    if(!a733_dma_native_init(&s->native,c->handoff->controller,c->handoff->controller,
        windows,3,NULL,0,c->fatal,c->fatal_cookie)) goto failed;
    unsigned job=0;
    for(unsigned ch=0;ch<A733_DMA_CHANNELS;++ch) if(c->handoff->service_channels&(1u<<ch)) {
        uintptr_t desc=s->extent.cpu+job*4096,src=desc+bank,dst=src+bank;
        uint64_t physical=s->extent.physical+job*4096;
        s->segments[job]=(struct a733_dma_segment){{physical+bank,0,4,8},
            {physical+2*bank,0,4,8},s->copy_bytes};
        s->mappings[job][0]=(struct a733_dma_mapping){(void *)src,physical+bank,4096,A733_DMA_READ};
        s->mappings[job][1]=(struct a733_dma_mapping){(void *)dst,physical+2*bank,4096,A733_DMA_WRITE};
        s->jobs[job]=(struct dma_service_job){.kind=DMA_SERVICE_RAM,.channel=ch,
            .transfer={{(void *)desc,physical,4096,A733_DMA_DESCRIPTORS},
                       &s->segments[job],1,s->mappings[job],2}};
        if(!exact_mapping(s,&s->jobs[job].transfer.descriptors) ||
           !exact_mapping(s,&s->mappings[job][0]) || !exact_mapping(s,&s->mappings[job][1])) goto failed;
        ++job;
    }
    }
    creating_bootstrap=1;
    s->bootstrap=xTaskCreateStaticAffinitySet(bootstrap,"dma-boot",A733_DMA_POOL_BOOT_STACK,s,
        c->priority,s->bootstrap_stack,&s->bootstrap_state,c->affinity);
    creating_bootstrap=0;
    if(!s->bootstrap || !a733_dma_startup_bind_task(&s->startup,s->bootstrap)) goto failed;
    const struct dma_service_protected protected[]={
        {0x41000000,0x41000000,pool_extent.cpu-0x41000000},
        {pool_extent.cpu+pool_extent.bytes,pool_extent.physical+pool_extent.bytes,
            0x42000000-pool_extent.cpu-pool_extent.bytes},
        {0x42000000,0x42000000,0x200000}};
    const struct dma_service_activation activation={s->bootstrap,prepared,s};
    if(!dma_service_init_deferred(&s->service,&operations,s,c->handoff->controller,
        c->handoff->service_channels,c->drain_ticks,s->jobs,s->count,protected,3,
        c->priority,c->affinity,&activation)) goto failed;
    atomic_store_explicit(&s->state,A733_DMA_POOL_WAITING,memory_order_release);
    return 1;
failed:
    device_memory_retain(pool,s->lease);
    atomic_store_explicit(&s->state,A733_DMA_POOL_FAILED,memory_order_release);
    return 0;
}

int a733_dma_pool_session_init(struct a733_dma_pool_session *s,const struct a733_dma_pool_config *c)
{return initialize(s,c,NULL,0,0,0);}
int a733_dma_pool_session_stream_init(struct a733_dma_pool_session *s,const struct a733_dma_pool_config *c,
    const struct dma_service_i2s_stream_profile *profile,unsigned periods,unsigned bytes)
{return profile?initialize(s,c,profile,periods,bytes,0):0;}
int a733_dma_pool_session_stream_supervised_init(struct a733_dma_pool_session *s,const struct a733_dma_pool_config *c,
    const struct dma_service_i2s_stream_profile *profile,unsigned periods,unsigned bytes)
{return profile?initialize(s,c,profile,periods,bytes,1):0;}
