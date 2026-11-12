/* Real pool, startup and service tasks; modeled A733 registers and DMA bus. */
#include "a733_dma_pool_session.h"
#include "a733_dma_regs.h"
#include "a733_i2s_platform.h"
#include "platform.h"
#include <string.h>

#define CCU 0x02002000u
#define DMA 0x04601000u
static struct a733_counter_clock clock_source;
static struct a733_clock_leases registry;
static struct a733_platform_locks locks;
static struct a733_dma_pool_session session;
static uint32_t ccu[0x2000/4],dma[0x2000/4];
static StaticTask_t client_state;
static StackType_t client_stack[3072];
static uint64_t frequency,initial_deadline;
static atomic_uint_fast64_t observed_deadline;
static atomic_uint busy_calls;
static atomic_uint platform_reads,device_accesses;
static unsigned platform_before,device_before;
static unsigned started,moved,invalidated;
static atomic_uint mask_calls,retirement_started;
static atomic_uint lock_held,release_lock,startup_busy;
static atomic_uint_fast64_t return_bound;
static StaticTask_t locker_state;
static StackType_t locker_stack[2048];
static struct a733_clock_token saved_token;
static struct a733_dma_platform_config saved_config;
static struct device_memory_allocation saved_allocation;
static struct a733_i2s_platform stream_binding;
static enum a733_i2s_resource_result unused_acquire(void *v,const struct a733_i2s_plan *p,uint64_t d)
{(void)v;(void)p;(void)d;platform_panic("unopened stream acquired",0);}
static enum a733_i2s_resource_result unused_resource(void *v,uint64_t d)
{(void)v;(void)d;platform_panic("unopened stream resource",0);}
static int unused_now(void *v,uint64_t *n)
{(void)v;(void)n;platform_panic("unopened stream clock",0);}
static int unused_read(void *v,uintptr_t a,uint32_t *n)
{(void)v;(void)a;(void)n;platform_panic("unopened stream read",0);}
static int unused_write(void *v,uintptr_t a,uint32_t n)
{(void)v;(void)a;(void)n;platform_panic("unopened stream write",0);}
static const struct dma_service_i2s_stream_profile stream_profile={
    .stream={{24576000,16000,0,16,32,1,0},
        {{unused_acquire,unused_resource,unused_resource,unused_now,unused_read,unused_write},unused_resource},
        &stream_binding,A733_I2S_RECEIVE,10000,1,A733_I2S_ROUTE_I2S0}};
extern const struct a733_dma_operations __real_a733_dma_native_operations;
static void client(void *);
static void locker(void *);
extern BaseType_t __real_a733_dma_pool_task_privileged(TaskFunction_t,void *);
BaseType_t __wrap_a733_dma_pool_task_privileged(TaskFunction_t entry,void *argument)
{
    return (!argument&&(entry==client||entry==locker))?pdTRUE:
        __real_a733_dma_pool_task_privileged(entry,argument);
}
extern int __real_a733_platform_lock_enter(struct a733_platform_locks *,enum a733_platform_lock,uint64_t);
int __wrap_a733_platform_lock_enter(struct a733_platform_locks *p,enum a733_platform_lock which,uint64_t deadline)
{
    if(DMA_RETURN_CASE==6&&xTaskGetCurrentTaskHandle()==session.bootstrap){
        uint64_t now;configASSERT(a733_counter_clock_now(&clock_source,&now));
        configASSERT(deadline<=now+(1000000u+configTICK_RATE_HZ-1)/configTICK_RATE_HZ);
    }
    int result=__real_a733_platform_lock_enter(p,which,deadline);
    if(!result&&xTaskGetCurrentTaskHandle()==session.bootstrap){
        configASSERT(xSemaphoreGetMutexHolder(locks.mutex[0])!=session.bootstrap);
        atomic_fetch_add_explicit(&startup_busy,1,memory_order_release);
    }
    return result;
}

static unsigned channel(unsigned offset) {return (0x100+offset)/4;}
static uint32_t *reg(uintptr_t address)
{
    if(address>=CCU && address<CCU+sizeof(ccu))return &ccu[(address-CCU)/4];
    configASSERT(address>=DMA && address<DMA+sizeof(dma));return &dma[(address-DMA)/4];
}
int __wrap_a733_dma_platform_native_read(void *cookie,uintptr_t address,uint32_t *out)
{
    (void)cookie;
    configASSERT(xTaskGetCurrentTaskHandle()==session.bootstrap);
    configASSERT(xSemaphoreGetMutexHolder(locks.mutex[0])==session.bootstrap);
    configASSERT(!atomic_load_explicit(&session.retired,memory_order_acquire));
    if(retirement_started)configASSERT(dma_service_retired(&session.service));
    ++platform_reads;*out=address==0x07090160 ? 0x10000002 : *reg(address);return 1;
}
int __wrap_a733_dma_platform_native_write(void *cookie,uintptr_t address,uint32_t value)
{
    (void)cookie;
    configASSERT((DMA_RETURN_CASE==4||DMA_RETURN_CASE==7)&&!retirement_started);
    configASSERT(xTaskGetCurrentTaskHandle()==session.bootstrap&&!session.startup.retired);
    configASSERT(address==CCU+0x704||address==CCU+0x5e4||address==DMA+0x28||address==DMA+0x34||address==DMA+0x38);
    *reg(address)=value;return 1;
}
static void worker(void)
{
    configASSERT(xTaskGetCurrentTaskHandle()==session.service.worker);
    configASSERT(!dma_service_retired(&session.service));
    configASSERT(!atomic_load_explicit(&session.retired,memory_order_acquire));
    ++device_accesses;
}
static uint32_t read_dma(void *cookie,uintptr_t address)
{
    configASSERT(cookie==&session.native);worker();
    if(started&&!moved){
        const uint32_t *d=(const void *)(uintptr_t)dma[channel(8)];
        configASSERT(d==(const void *)session.jobs[0].transfer.descriptors.cpu);
        configASSERT(d[1]==session.mappings[0][0].bus&&d[2]==session.mappings[0][1].bus&&d[3]==1024);
        memcpy((void *)(uintptr_t)d[2],(const void *)(uintptr_t)d[1],d[3]);
        dma[channel(0x38)]|=4;moved=1;
    }
    if(started&&!dma[channel(0)])dma[0x30/4]=0;
    return *reg(address);
}
static void write_dma(void *cookie,uintptr_t address,uint32_t value)
{
    configASSERT(cookie==&session.native);worker();
    configASSERT(address>=DMA+0x100&&address<DMA+0x140);
    unsigned r=address-DMA-0x100;
    if(r==0&&value){started=1;dma[0x30/4]=1;}
    if(r==0x38)*reg(address)&=~value;else *reg(address)=value;
}
static void cache(void *cookie,enum a733_dma_cache op,void *cpu,size_t bytes)
{
    worker();
    if(op==A733_DMA_INVALIDATE){configASSERT(moved&&!dma[channel(0)]&&!dma[0x30/4]);++invalidated;}
    __real_a733_dma_native_operations.cache(cookie,op,cpu,bytes);
}
static void barrier(void *cookie){worker();__real_a733_dma_native_operations.barrier(cookie);}
static uint64_t ticks(void *cookie){worker();return __real_a733_dma_native_operations.ticks(cookie);}
static int mapping(void *cookie,const struct a733_dma_mapping *m)
{return __real_a733_dma_native_operations.validate_mapping(cookie,m);}
static int fifo(void *cookie,const struct a733_dma_endpoint *e,enum a733_dma_access a)
{return __real_a733_dma_native_operations.validate_fifo(cookie,e,a);}
const struct a733_dma_operations __wrap_a733_dma_native_operations={
    read_dma,write_dma,cache,barrier,ticks,mapping,fifo};
extern enum a733_dma_platform_result __real_a733_dma_startup_retire(struct a733_dma_startup *,uint64_t);
enum a733_dma_platform_result __wrap_a733_dma_startup_retire(struct a733_dma_startup *startup,uint64_t deadline)
{
    uint64_t old=atomic_load_explicit(&observed_deadline,memory_order_acquire);
    configASSERT(!old||old==deadline);
    atomic_store_explicit(&observed_deadline,deadline,memory_order_release);
    enum a733_dma_platform_result result=__real_a733_dma_startup_retire(startup,deadline);
    if(result==A733_DMA_PLATFORM_BUSY)atomic_fetch_add_explicit(&busy_calls,1,memory_order_release);
    return result;
}
static void preserved(void)
{
    configASSERT(!memcmp(&saved_token,&session.startup.token,sizeof(saved_token)));
    configASSERT(!memcmp(&saved_token,&session.startup.platform.token,sizeof(saved_token)));
    configASSERT(!memcmp(&saved_config,&session.startup.platform.config,sizeof(saved_config)));
    configASSERT(session.startup.handoff.allowed.deadline_us==initial_deadline);
    configASSERT(!memcmp(&saved_allocation,&session.pool->allocations[session.lease.slot],sizeof(saved_allocation)));
    configASSERT(saved_allocation.state==DEVICE_MEMORY_DEVICE&&device_accesses==device_before&&mask_calls==1);
}
static _Noreturn void fatal(void *unused)
{
    (void)unused;
    configASSERT(DMA_RETURN_CASE==1||DMA_RETURN_CASE==2);
    configASSERT(retirement_started&&xTaskGetCurrentTaskHandle()==session.bootstrap);
    configASSERT(atomic_load_explicit(&session.state,memory_order_acquire)==A733_DMA_POOL_FAILED);
    configASSERT(!atomic_load_explicit(&session.retired,memory_order_acquire)&&!session.startup.retired);
    configASSERT(dma_service_retired(&session.service)&&session.startup.failed);
    preserved();
    if(DMA_RETURN_CASE==1){
        configASSERT(session.startup.retire_fault==4&&platform_reads>platform_before);
        configASSERT(session.startup.platform.retire_fault==A733_DMA_RETIRE_CONTROLLER);
        configASSERT(session.startup.retire_read_address==DMA+A733_DMA_REG_CPU_ROUTE);
        configASSERT(session.startup.retire_read_result==1&&session.startup.retire_read_value==0);
        configASSERT(session.startup.retire_token_result==A733_CLOCK_OK);
        platform_puts("PASS DMA pool return: changed route refuses final acknowledgment\n");
    }else{
        configASSERT(observed_deadline&&session.startup.retire_deadline==observed_deadline);
        configASSERT(session.startup.retire_fault==2&&session.startup.retire_now>=observed_deadline);
        configASSERT(platform_reads==platform_before);
        configASSERT(session.startup.retire_token_result==A733_CLOCK_BUSY);
        configASSERT(!session.startup.platform.retire_fault&&!session.startup.retire_read_address);
        platform_puts("PASS DMA pool return: registry BUSY expires within fixed 50000 us\n");
    }
    platform_exit(0);
}
static int rearm(void *cookie)
{configASSERT(cookie==&session&&xTaskGetCurrentTaskHandle()==session.service.worker);return 1;}
static int mask(void *cookie)
{
    configASSERT(cookie==&session&&xTaskGetCurrentTaskHandle()==session.service.worker);
    configASSERT(!dma_service_retired(&session.service));
    for(unsigned i=0;i<A733_DMA_CHANNELS;++i)configASSERT(session.service.channels[i].phase==DMA_SERVICE_CLOSED);
    ++mask_calls;return 1;
}
static struct dma_service_reply request(enum dma_service_operation op,uint64_t timeout)
{
    struct dma_service_request request={.operation=op,.timeout=timeout};
    struct dma_service_reply reply;uint64_t ticket;
    configASSERT(dma_service_send(&session.service,&request,100,&ticket));
    configASSERT(dma_service_collect(&session.service,ticket,&reply,100));return reply;
}
static void client(void *unused)
{
    (void)unused;
    if(DMA_RETURN_CASE==6){
        while(!atomic_load_explicit(&lock_held,memory_order_acquire))vTaskDelay(1);
        while(!atomic_load_explicit(&startup_busy,memory_order_acquire))vTaskDelay(1);
        uint64_t now;configASSERT(a733_counter_clock_now(&clock_source,&now));
        return_bound=now+5000;
        configASSERT(a733_dma_pool_session_return_limit(&session,return_bound)==1);
        configASSERT(a733_dma_pool_session_return_limit(&session,return_bound+1)==-1);
        for(unsigned i=0;i<100&&atomic_load_explicit(&session.state,memory_order_acquire)==A733_DMA_POOL_WAITING;++i)vTaskDelay(1);
        configASSERT(atomic_load_explicit(&session.state,memory_order_acquire)==A733_DMA_POOL_FAILED);
        configASSERT(!atomic_load(&session.retired)&&!session.startup.register_held&&!platform_reads&&!device_accesses);
        configASSERT(session.startup.token.id&&session.pool->allocations[session.lease.slot].state==DEVICE_MEMORY_DEVICE);
        configASSERT(session.startup.handoff.allowed.deadline_us==initial_deadline&&initial_deadline>return_bound);
        configASSERT(xSemaphoreGetMutexHolder(locks.mutex[0])!=session.bootstrap);
        atomic_store_explicit(&release_lock,1,memory_order_release);
        for(unsigned i=0;i<3;++i)vTaskDelay(1);
        configASSERT(!xSemaphoreGetMutexHolder(locks.mutex[0])&&!platform_reads&&!device_accesses);
        platform_puts("PASS DMA pool return: startup mutex wait observes shared return bound and retains untouched allocation\n");
        platform_exit(0);
    }
    for(unsigned i=0;i<1000&&atomic_load_explicit(&session.state,memory_order_acquire)==A733_DMA_POOL_WAITING;++i)vTaskDelay(1);
    configASSERT(atomic_load_explicit(&session.state,memory_order_acquire)==A733_DMA_POOL_READY);
    saved_token=session.startup.token;saved_config=session.startup.platform.config;
    saved_allocation=session.pool->allocations[session.lease.slot];
    struct dma_service_reply reply={0};
    if(DMA_RETURN_CASE!=7){
        unsigned char *src=session.mappings[0][0].cpu,*dst=session.mappings[0][1].cpu;
        for(unsigned i=0;i<4096;++i){src[i]=(unsigned char)(i*29);dst[i]=0xa5;}
        configASSERT(request(DMA_SERVICE_OPEN,0).result==DMA_SERVICE_OK);
        configASSERT(request(DMA_SERVICE_SUBMIT,frequency/100).result==DMA_SERVICE_OK);
        for(unsigned i=0;i<1000;++i){reply=request(DMA_SERVICE_STATUS,0);if(reply.phase==DMA_SERVICE_RESULT)break;vTaskDelay(1);}
        configASSERT(reply.result==DMA_SERVICE_OK&&reply.outcome==DMA_SERVICE_COMPLETE&&invalidated==1);
        for(unsigned i=0;i<4096;++i)configASSERT(dst[i]==(i<1024?src[i]:0xa5));
        configASSERT(request(DMA_SERVICE_REAP,0).result==DMA_SERVICE_OK);
        configASSERT(request(DMA_SERVICE_CLOSE,0).phase==DMA_SERVICE_CLOSED);
    }else configASSERT(session.periods==4&&session.service.channels[0].phase==DMA_SERVICE_CLOSED);
    configASSERT(a733_dma_pool_session_retire(&session)==-1);
    struct dma_service_request retire={.operation=DMA_SERVICE_RETIRE};uint64_t ticket;
    configASSERT(dma_service_send(&session.service,&retire,100,&ticket));
    for(unsigned i=0;i<1000&&!mask_calls;++i)vTaskDelay(1);
    configASSERT(mask_calls==1&&!dma_service_retired(&session.service));
    configASSERT(!atomic_load_explicit(&session.service.retirement_collected,memory_order_acquire));
    configASSERT(a733_dma_pool_session_retire(&session)==-1);
    configASSERT(dma_service_collect(&session.service,ticket,&reply,100)&&reply.result==DMA_SERVICE_OK);
    for(unsigned i=0;i<1000&&!dma_service_retired(&session.service);++i)vTaskDelay(1);
    configASSERT(dma_service_retired(&session.service));
    device_before=device_accesses;platform_before=platform_reads;retirement_started=1;
    if(DMA_RETURN_CASE==1)dma[0x34/4]^=1;
    if(DMA_RETURN_CASE==2||DMA_RETURN_CASE==3)atomic_store_explicit(&registry.busy,1,memory_order_release);
    uint64_t before;configASSERT(a733_counter_clock_now(&clock_source,&before));
    if(DMA_RETURN_CASE==5){
        return_bound=before+10000;
        configASSERT(a733_dma_pool_session_return_limit(&session,return_bound)==1);
        configASSERT(a733_dma_pool_session_return_limit(&session,return_bound)==1);
        configASSERT(a733_dma_pool_session_return_limit(&session,return_bound+1)==-1);
        configASSERT(a733_dma_pool_session_return_limit(&session,before)==-1);
    }
    configASSERT(a733_dma_pool_session_retire(&session)==0);
    for(unsigned i=0;i<1000&&!atomic_load_explicit(&observed_deadline,memory_order_acquire);++i)vTaskDelay(1);
    if(DMA_RETURN_CASE==5)configASSERT(observed_deadline==return_bound);
    else configASSERT(observed_deadline>=before+50000&&observed_deadline<before+100000);
    if(DMA_RETURN_CASE==2||DMA_RETURN_CASE==3){
        for(unsigned i=0;i<3;++i){
            vTaskDelay(1);configASSERT(atomic_load_explicit(&busy_calls,memory_order_acquire)>0);
            configASSERT(!atomic_load_explicit(&session.retired,memory_order_acquire)&&platform_reads==platform_before);
        }
        if(DMA_RETURN_CASE==3)atomic_store_explicit(&registry.busy,0,memory_order_release);
    }
    for(unsigned i=0;i<1000&&!atomic_load_explicit(&session.retired,memory_order_acquire);++i)vTaskDelay(1);
    configASSERT(DMA_RETURN_CASE==0||DMA_RETURN_CASE==3||DMA_RETURN_CASE==4||DMA_RETURN_CASE==5||DMA_RETURN_CASE==7);
    configASSERT(a733_dma_pool_session_retire(&session)==1&&session.startup.retired);
    configASSERT(!session.startup.retire_fault&&!session.startup.platform.retire_fault);
    configASSERT(session.startup.retire_token_result==A733_CLOCK_OK);
    configASSERT(session.startup.retire_deadline==observed_deadline&&platform_reads>platform_before);
    configASSERT(!session.startup.register_held&&xSemaphoreGetMutexHolder(locks.mutex[0])==NULL);
    preserved();unsigned last=platform_reads;
    for(unsigned i=0;i<10;++i){vTaskDelay(1);configASSERT(a733_dma_pool_session_retire(&session)==1);preserved();configASSERT(platform_reads==last);}
    platform_puts(DMA_RETURN_CASE==7?
        "PASS DMA pool return: unopened stream keeps descriptor/period allocation and retires cold controller\n":DMA_RETURN_CASE==4?
        "PASS DMA pool return: cold-started controller retains admitted initial policy and verifies final routes\n":DMA_RETURN_CASE==5?
        "PASS DMA pool return: shared return bound clamps final verification and refuses extension\n":DMA_RETURN_CASE==3?
        "PASS DMA pool return: transient registry BUSY preserves deadline and completes\n":
        "PASS DMA pool return: copy, CLOSE, collected RETIRE, worker acknowledgment and final bootstrap acknowledgment\n");
    platform_exit(0);
}
static void locker(void *unused)
{
    (void)unused;
    configASSERT(a733_platform_lock_enter(&locks,A733_PLATFORM_REGISTER_LOCK,initial_deadline));
    configASSERT(a733_dma_pool_session_return_limit(&session,initial_deadline)==-1);
    atomic_store_explicit(&lock_held,1,memory_order_release);
    while(!atomic_load_explicit(&release_lock,memory_order_acquire))vTaskDelay(1);
    a733_platform_lock_leave(&locks,A733_PLATFORM_REGISTER_LOCK);
    vTaskSuspend(NULL);platform_panic("locker resumed",0);
}
void vApplicationStackOverflowHook(TaskHandle_t task,char *name)
{(void)task;platform_panic(name,0);}
void kernel_main(void)
{
    platform_init();__asm__ volatile("mrs %0,cntfrq_el0":"=r"(frequency));
    configASSERT(frequency&&frequency<=UINT32_MAX);
    configASSERT(a733_counter_clock_init(&clock_source,(uint32_t)frequency));
    configASSERT(a733_clock_leases_init(&registry,a733_counter_clock_now,&clock_source));
    configASSERT(a733_platform_locks_init(&locks,a733_counter_clock_now,&clock_source));
    uint64_t now;configASSERT(a733_counter_clock_now(&clock_source,&now));initial_deadline=now+2000000;
    struct a733_dma_handoff handoff={
        .allowed={.fabric={0x03000002,0x82000001,0x84000001},.rate_hz={200000000,400000000,200000000},
            .expected_cpu=1,.desired_cpu=1,.owner=7,.handoff=123,.deadline_us=initial_deadline},
        .controller=DMA,.evidence=123,.owned_channels=0xffff,.service_channels=1,.established=A733_DMA_HANDOFF_REQUIRED};
    if(DMA_RETURN_CASE==4||DMA_RETURN_CASE==7){handoff.allowed.cold=1;handoff.allowed.expected_cpu=0;handoff.allowed.route_change_mask=1;}
    const struct a733_dma_pool_config config={&handoff,&clock_source,&registry,&locks,frequency/100,1024,4,1,fatal,NULL};
    configASSERT(DMA_RETURN_CASE==7?
        a733_dma_pool_session_stream_init(&session,&config,&stream_profile,4,4096):
        a733_dma_pool_session_init(&session,&config));
    configASSERT(session.count==1&&session.extent.bytes==(DMA_RETURN_CASE==7?5u:3u)*4096);
    const struct dma_service_irq irq={rearm,&session,1,frequency/configTICK_RATE_HZ};
    configASSERT(dma_service_bind_irq(&session.service,&irq));
    configASSERT(dma_service_bind_retirement(&session.service,mask,&session));
    ccu[0]=0xf8000000|(99<<16)|(99<<8);
    ccu[0xa0/4]=0xfe000000|(1<<20)|(2<<16)|(99<<8)|(4<<2);
    ccu[0x1908/4]=0x8fff0fff;ccu[0x1988/4]=0x0fff0000;
    for(unsigned i=0;i<3;++i)ccu[(i==0?0x500:i==1?0x580:0x588)/4]=handoff.allowed.fabric[i];
    ccu[0x5e4/4]=0x40303;ccu[0x704/4]=0x10001;dma[0x34/4]=1;dma[0x28/4]=4;
    if(DMA_RETURN_CASE==4||DMA_RETURN_CASE==7){ccu[0x5e4/4]&=~1u;ccu[0x704/4]=0;dma[0x34/4]=0;dma[0x28/4]=0;}
    if(DMA_RETURN_CASE==6)configASSERT(xTaskCreateStaticAffinitySet(locker,"return-lock",2048,NULL,6,locker_stack,&locker_state,1));
    TaskHandle_t task=xTaskCreateStaticAffinitySet(client,"return-client",3072,NULL,2,client_stack,&client_state,2);
    configASSERT(task&&dma_service_bind(&session.service,0,task,1));
    vTaskStartScheduler();platform_panic("scheduler returned",0);
}
