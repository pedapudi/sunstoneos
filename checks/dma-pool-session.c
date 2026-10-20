/* Actual native memory/startup/service composition on two QEMU CPUs. Only
 * A733 register behavior and DMA bus movement are modeled. */
#include "a733_dma_pool_session.h"
#include "platform.h"
#include <string.h>

#define CCU 0x02002000u
#define DMA 0x04601000u
static struct a733_counter_clock clock_source;
static struct a733_clock_leases registry;
static struct a733_platform_locks locks;
static struct a733_dma_pool_session session,copied;
static uint32_t ccu[0x2000/4],dma[0x2000/4];
static StaticTask_t client_state;
static StackType_t client_stack[3072];
static uint64_t frequency;
static unsigned platform_accesses,device_accesses,cleaned[2][3],invalidated[2],started[2],moved;
static unsigned fault_accesses,cache_calls,fault_cache_calls;
static atomic_uint expect_fault;
static struct device_memory_lease neighbor_lease;
static struct device_memory_extent neighbor;
extern const struct a733_dma_operations __real_a733_dma_native_operations;

static unsigned channel_reg(unsigned ch,unsigned offset)
{ return (0x100+ch*0x40+offset)/4; }
static uint32_t *reg(uintptr_t address)
{
    if(address>=CCU && address<CCU+sizeof(ccu)) return &ccu[(address-CCU)/4];
    configASSERT(address>=DMA && address<DMA+sizeof(dma));return &dma[(address-DMA)/4];
}
int __wrap_a733_dma_platform_native_read(void *cookie,uintptr_t address,uint32_t *out)
{
    (void)cookie;
    configASSERT(xTaskGetCurrentTaskHandle()==session.bootstrap);
    configASSERT(xSemaphoreGetMutexHolder(locks.mutex[0])==session.bootstrap);
    ++platform_accesses;*out=address==0x07090160 ? 0x10000002 : *reg(address);return 1;
}
int __wrap_a733_dma_platform_native_write(void *cookie,uintptr_t address,uint32_t value)
{
    (void)cookie;
    configASSERT(xTaskGetCurrentTaskHandle()==session.bootstrap);
    configASSERT(xSemaphoreGetMutexHolder(locks.mutex[0])==session.bootstrap);
    ++platform_accesses;*reg(address)=value;return 1;
}
static void worker(void)
{
    uint64_t el,sp;__asm__ volatile("mrs %0,CurrentEL\n mrs %1,spsel":"=r"(el),"=r"(sp));
    configASSERT(el==4 && sp==0 && xTaskGetCurrentTaskHandle()==session.service.worker);
    configASSERT(a733_dma_startup_ready(&session.startup));
}
static void advance(void)
{
    if(started[0] && !moved) {
        const uint32_t *descriptor=(const void *)(uintptr_t)dma[channel_reg(0,8)];
        configASSERT(descriptor==(const void *)session.jobs[0].transfer.descriptors.cpu);
        configASSERT(descriptor[1]==session.mappings[0][0].bus && descriptor[2]==session.mappings[0][1].bus);
        configASSERT(descriptor[3]==1024 && descriptor[4]==8 && descriptor[5]==0xfffff800);
        memcpy((void *)(uintptr_t)descriptor[2],(const void *)(uintptr_t)descriptor[1],descriptor[3]);
        dma[channel_reg(0,0x38)]|=4;moved=1;
    }
    if(started[0] && !dma[channel_reg(0,0)]) dma[0x30/4]&=~1u;
    /* Channel one retains BUSY after disable: accepted writes have no modeled
     * completion guarantee, so its destination cannot become CPU-owned. */
}
static uint32_t read_dma(void *cookie,uintptr_t address)
{
    configASSERT(cookie==&session.native);worker();++device_accesses;
    advance();return *reg(address);
}
static void write_dma(void *cookie,uintptr_t address,uint32_t value)
{
    configASSERT(cookie==&session.native);worker();++device_accesses;
    configASSERT(address>=DMA+0x100 && address<DMA+0x180);
    unsigned offset=address-DMA,ch=(offset-0x100)/0x40,r=(offset-0x100)%0x40;
    if(r==0 && value) {
        configASSERT(cleaned[ch][0] && cleaned[ch][1] && cleaned[ch][2]);
        dma[0x30/4]|=1u<<ch;started[ch]=1;
    }
    if(r==0x38) dma[offset/4]&=~value;
    else dma[offset/4]=value;
}
static void cache(void *cookie,enum a733_dma_cache op,void *cpu,size_t bytes)
{
    configASSERT(cookie==&session.native);worker();configASSERT(bytes==4096);
    ++cache_calls;
    size_t index=((uintptr_t)cpu-session.extent.cpu)/4096;
    unsigned bank=(unsigned)index/2,ch=(unsigned)index%2;
    configASSERT(index<6 && (uintptr_t)cpu==session.extent.cpu+index*4096);
    if(op==A733_DMA_INVALIDATE) {
        configASSERT(bank==2 && ch==0 && !dma[channel_reg(ch,0)] && !(dma[0x30/4]&(1u<<ch)));
        configASSERT(dma[channel_reg(ch,8)]==0);++invalidated[ch];
    } else {configASSERT(op==(bank==2 ? A733_DMA_CLEAN_INVALIDATE : A733_DMA_CLEAN));++cleaned[ch][bank];}
    __real_a733_dma_native_operations.cache(cookie,op,cpu,bytes);
}
static void barrier(void *cookie)
{ __real_a733_dma_native_operations.barrier(cookie); }
static uint64_t ticks(void *cookie)
{ return __real_a733_dma_native_operations.ticks(cookie); }
static int mapping(void *cookie,const struct a733_dma_mapping *m)
{ return __real_a733_dma_native_operations.validate_mapping(cookie,m); }
static int fifo(void *cookie,const struct a733_dma_endpoint *e,enum a733_dma_access a)
{ return __real_a733_dma_native_operations.validate_fifo(cookie,e,a); }
const struct a733_dma_operations __wrap_a733_dma_native_operations={
    read_dma,write_dma,cache,barrier,ticks,mapping,fifo};
static _Noreturn void fatal(void *unused)
{
    (void)unused;
    if(atomic_load_explicit(&expect_fault,memory_order_acquire)) {
        configASSERT(xTaskGetCurrentTaskHandle()==session.service.worker);
        configASSERT(atomic_load_explicit(&registry.failed,memory_order_acquire));
        configASSERT(atomic_load_explicit(&session.state,memory_order_acquire)==A733_DMA_POOL_FAILED);
        configASSERT(device_accesses==fault_accesses && cache_calls==fault_cache_calls &&
            invalidated[0]==1 && !invalidated[1]);
        configASSERT(session.pool->allocations[session.lease.slot].state==DEVICE_MEMORY_DEVICE);
        configASSERT(a733_dma_owns(&session.service.controller,&session.service.jobs[1].transfer));
        configASSERT(session.service.channels[1].holds_buffers && session.service.channels[1].phase==DMA_SERVICE_RECOVERY);
        platform_puts("PASS DMA pool session: native reservations, startup and cross-core copied jobs; completed copy/reap, uncertain drain and late authority fault retain memory\n");
        platform_exit(0);
    }
    platform_panic("DMA pool callback",0);
}
static unsigned level(void)
{
    register uint64_t result __asm__("x0");
    __asm__ volatile("svc #120":"=r"(result)::"memory");return (unsigned)result;
}
static struct dma_service_reply request(enum dma_service_operation op,unsigned job,uint64_t timeout)
{
    configASSERT(level()==0);
    struct dma_service_request r={.operation=op,.job=job,.timeout=timeout};
    struct dma_service_reply reply;uint64_t ticket;
    configASSERT(dma_service_send(&session.service,&r,100,&ticket));
    configASSERT(dma_service_collect(&session.service,ticket,&reply,100));
    return reply;
}
static void client(void *unused)
{
    (void)unused;
    unsigned state=A733_DMA_POOL_WAITING;
    for(unsigned i=0;i<1000 && state==A733_DMA_POOL_WAITING;++i) {
        state=atomic_load_explicit(&session.state,memory_order_acquire);
        if(state==A733_DMA_POOL_WAITING) vTaskDelay(1);
    }
    configASSERT(state==A733_DMA_POOL_READY && level()==0 && platform_accesses);
    for(unsigned j=0;j<2;++j) {
        unsigned char *src=session.mappings[j][0].cpu,*dst=session.mappings[j][1].cpu;
        for(unsigned i=0;i<4096;++i) {src[i]=(unsigned char)(i*29+j);dst[i]=0xa5;}
        struct dma_service_reply r=request(DMA_SERVICE_OPEN,j,0);
        configASSERT(r.result==DMA_SERVICE_OK && r.phase==DMA_SERVICE_READY);
        r=request(DMA_SERVICE_SUBMIT,j,frequency/100);
        configASSERT(r.result==DMA_SERVICE_OK);
        for(unsigned i=0;i<1000 && r.phase!=DMA_SERVICE_RESULT && r.phase!=DMA_SERVICE_RECOVERY;++i) {
            vTaskDelay(1);r=request(DMA_SERVICE_STATUS,j,0);
        }
        if(!j) {
            configASSERT(r.result==DMA_SERVICE_OK && r.phase==DMA_SERVICE_RESULT &&
                r.outcome==DMA_SERVICE_COMPLETE && r.holds_buffers && invalidated[0]==1);
            for(unsigned i=0;i<4096;++i) configASSERT(dst[i]==(i<1024 ? src[i] : 0xa5));
            configASSERT(!a733_dma_owns(&session.service.controller,&session.service.jobs[0].transfer));
            configASSERT(request(DMA_SERVICE_CLOSE,0,0).result==DMA_SERVICE_BUSY);
            r=request(DMA_SERVICE_REAP,0,0);
            configASSERT(r.result==DMA_SERVICE_OK && r.phase==DMA_SERVICE_READY && !r.holds_buffers);
            configASSERT(request(DMA_SERVICE_CLOSE,0,0).phase==DMA_SERVICE_CLOSED);
        } else {
            configASSERT(r.result==DMA_SERVICE_FAULT && r.phase==DMA_SERVICE_RECOVERY && r.holds_buffers);
            configASSERT(!invalidated[1] && a733_dma_owns(&session.service.controller,&session.service.jobs[1].transfer));
            configASSERT(request(DMA_SERVICE_REAP,1,0).result==DMA_SERVICE_FAULT);
            configASSERT(request(DMA_SERVICE_CLOSE,1,0).result==DMA_SERVICE_FAULT);
            /* Never read the destination after uncertain drain. */
        }
    }
    configASSERT(session.pool->allocations[session.lease.slot].state==DEVICE_MEMORY_DEVICE);
    configASSERT(session.pool->allocations[session.lease.slot].generation==session.lease.generation);
    configASSERT(session.startup.token.id && device_accesses && moved);
    /* Fixture-only authority fault injection through the actual registry.
     * Production clients never receive this private token or registry. */
    fault_accesses=device_accesses;fault_cache_calls=cache_calls;
    atomic_store_explicit(&expect_fault,1,memory_order_release);
    configASSERT(a733_clock_retain(&registry,&session.startup.token)==A733_CLOCK_FAULT);
    (void)request(DMA_SERVICE_OPEN,0,0);
    platform_panic("authority fault returned to client",0);
}
void vApplicationStackOverflowHook(TaskHandle_t task,char *name)
{ (void)task;platform_panic(name,0); }
void kernel_main(void)
{
    platform_init();__asm__ volatile("mrs %0,cntfrq_el0":"=r"(frequency));
    configASSERT(frequency && frequency<=UINT32_MAX);
    configASSERT(a733_counter_clock_init(&clock_source,(uint32_t)frequency));
    configASSERT(a733_clock_leases_init(&registry,a733_counter_clock_now,&clock_source));
    configASSERT(a733_platform_locks_init(&locks,a733_counter_clock_now,&clock_source));
    struct device_memory_pool *pool=a733_memory_reservations_init();configASSERT(pool);
    configASSERT(device_memory_acquire(pool,99,4096,&neighbor_lease,&neighbor));
    device_memory_lookup(pool,neighbor_lease)->state=DEVICE_MEMORY_DEVICE;
    uint64_t now;configASSERT(a733_counter_clock_now(&clock_source,&now));
    struct a733_dma_handoff handoff={
        .allowed={.fabric={0x03000002,0x82000001,0x84000001},.rate_hz={200000000,400000000,200000000},
            .desired_cpu=3,.route_change_mask=3,.owner=7,.handoff=123,.deadline_us=now+2000000,.cold=1},
        .controller=DMA,.evidence=123,.owned_channels=0xffff,.service_channels=3};
    const struct a733_dma_pool_config config={&handoff,&clock_source,&registry,&locks,
        frequency/100,1024,4,1,fatal,NULL};
    configASSERT(!a733_dma_pool_session_init(&session,&config));
    configASSERT(!session.admitted && !platform_accesses && !device_accesses);
    handoff.established=A733_DMA_HANDOFF_REQUIRED;
    configASSERT(a733_dma_pool_session_init(&session,&config));
    configASSERT(session.count==2 && session.extent.bytes==6*4096);
    configASSERT(!device_memory_release(pool,session.lease));
    const struct a733_dma_operations *ops=&session.service.controller.operations;
    struct a733_dma_mapping wrong={(void *)neighbor.cpu,neighbor.physical,4096,A733_DMA_READ};
    configASSERT(!ops->validate_mapping(&session,&wrong));
    wrong=session.mappings[0][0];wrong.bytes=64;
    configASSERT(!ops->validate_mapping(&session,&wrong));
    wrong=session.mappings[0][0];wrong.access=A733_DMA_WRITE;
    configASSERT(!ops->validate_mapping(&session,&wrong));
    wrong=session.mappings[0][0];wrong.bus+=4096;
    configASSERT(!ops->validate_mapping(&session,&wrong));
    copied=session;
    configASSERT(!ops->validate_mapping(&copied,&session.mappings[0][0]));
    ++session.lease.generation;
    configASSERT(!ops->validate_mapping(&session,&session.mappings[0][0]));
    --session.lease.generation;
    configASSERT(ops->validate_mapping(&session,&session.mappings[0][0]));
    configASSERT(!platform_accesses && !device_accesses);
    ccu[0]=0xf8000000|(99<<16)|(99<<8);
    ccu[0xa0/4]=0xfe000000|(1<<20)|(2<<16)|(99<<8)|(4<<2);
    ccu[0x1908/4]=0x8fff0fff;ccu[0x1988/4]=0x0fff0000;
    ccu[0x500/4]=handoff.allowed.fabric[0];ccu[0x580/4]=handoff.allowed.fabric[1];ccu[0x588/4]=handoff.allowed.fabric[2];
    ccu[0x5e4/4]=0x40302;
    memset(&handoff,0,sizeof(handoff));
    TaskHandle_t task=xTaskCreateStaticAffinitySet(client,"pool-client",3072,NULL,2,client_stack,&client_state,2);
    configASSERT(task && dma_service_bind(&session.service,0,task,3));
    vTaskStartScheduler();platform_panic("scheduler returned",0);
}
