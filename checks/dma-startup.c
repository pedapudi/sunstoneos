#include "a733_dma_startup.h"
#include "dma_service.h"
#include "platform.h"
#include <string.h>

#define CCU 0x02002000u
#define DMA 0x04601000u
static struct a733_counter_clock clock_source,bad_clock;
static struct a733_clock_leases registry,bad_registry;
static struct a733_platform_locks locks,bad_locks;
static struct a733_dma_startup startup,bad_auth,bad_time;
static struct dma_service service;
static StaticTask_t task_storage[3];
static StackType_t stacks[3][3072];
static TaskHandle_t bootstrap_task,client_task,holder_task;
static uint32_t ccu[0x2000/4],dma[0x500/4];
static atomic_uint platform_reads,platform_writes,service_calls,holder_done;
static unsigned retained_mode,prepared_calls;
static _Alignas(64) unsigned char pool[192];
static uint64_t frequency;

static uint32_t *reg(uintptr_t address)
{
    if(address>=CCU && address<CCU+sizeof(ccu)) return &ccu[(address-CCU)/4];
    configASSERT(address>=DMA && address<DMA+sizeof(dma));return &dma[(address-DMA)/4];
}
int __wrap_a733_dma_platform_native_read(void *cookie,uintptr_t address,uint32_t *out)
{
    (void)cookie;
    configASSERT(xTaskGetCurrentTaskHandle()==bootstrap_task);
    configASSERT(xSemaphoreGetMutexHolder(locks.mutex[0])==bootstrap_task);
    atomic_fetch_add_explicit(&platform_reads,1,memory_order_relaxed);
    *out=address==0x07090160 ? 0x10000002 : *reg(address);return 1;
}
int __wrap_a733_dma_platform_native_write(void *cookie,uintptr_t address,uint32_t value)
{
    (void)cookie;
    configASSERT(xTaskGetCurrentTaskHandle()==bootstrap_task);
    configASSERT(xSemaphoreGetMutexHolder(locks.mutex[0])==bootstrap_task);
    atomic_fetch_add_explicit(&platform_writes,1,memory_order_relaxed);
    *reg(address)=value;
    return !(retained_mode==1 && address==CCU+0x704);
}
static uint32_t read_dma(void *cookie,uintptr_t address)
{
    (void)cookie;configASSERT(a733_dma_startup_ready(&startup));
    atomic_fetch_add_explicit(&service_calls,1,memory_order_relaxed);return *reg(address);
}
static void write_dma(void *cookie,uintptr_t address,uint32_t value)
{(void)cookie;(void)address;(void)value;configASSERT(0);}
static void cache(void *cookie,enum a733_dma_cache operation,void *cpu,size_t bytes)
{(void)cookie;(void)operation;(void)cpu;(void)bytes;configASSERT(0);}
static void barrier(void *cookie) {(void)cookie;__asm__ volatile("dsb sy" ::: "memory");}
static uint64_t ticks(void *cookie)
{(void)cookie;uint64_t v;__asm__ volatile("isb\n mrs %0,cntpct_el0":"=r"(v)::"memory");return v;}
static int mapping(void *cookie,const struct a733_dma_mapping *m) {(void)cookie;(void)m;return 1;}
static int fifo(void *cookie,const struct a733_dma_endpoint *e,enum a733_dma_access a)
{(void)cookie;(void)e;(void)a;return 0;}
static int prepared(void *cookie,uintptr_t controller,uint16_t channels)
{++prepared_calls;return a733_dma_startup_prepared(cookie,controller,channels);}
static uint64_t deadline(void)
{uint64_t now;configASSERT(a733_counter_clock_now(&clock_source,&now));return now+1000000;}
static void holder(void *unused)
{
    (void)unused;configASSERT(ulTaskNotifyTake(pdTRUE,1000)==1);
    configASSERT(a733_platform_lock_enter(&locks,A733_PLATFORM_REGISTER_LOCK,deadline()));
    xTaskNotifyGive(bootstrap_task);vTaskDelay(3);
    ccu[0x704/4]|=2;
    atomic_store_explicit(&holder_done,1,memory_order_release);
    a733_platform_lock_leave(&locks,A733_PLATFORM_REGISTER_LOCK);
    vTaskSuspend(NULL);configASSERT(0);
}
static void client(void *unused)
{
    (void)unused;
    struct dma_service_request request={DMA_SERVICE_OPEN,0,0,0};uint64_t ticket=0xabc;
    configASSERT(!dma_service_send(&service,&request,0,&ticket) && ticket==0xabc);
    configASSERT(dma_service_activate(&service)==DMA_SERVICE_DENIED);
    configASSERT(!prepared_calls && !atomic_load(&service_calls) && !atomic_load(&platform_reads));
    xTaskNotifyGive(bootstrap_task);
    configASSERT(ulTaskNotifyTake(pdTRUE,1000)==1);
    if(retained_mode) {
        configASSERT(!dma_service_send(&service,&request,0,&ticket) && ticket==0xabc);
        configASSERT(dma_service_activate(&service)==DMA_SERVICE_DENIED);
        configASSERT(!atomic_load(&service_calls));
    } else {
        configASSERT(dma_service_send(&service,&request,10,&ticket));
        struct dma_service_reply reply;
        configASSERT(dma_service_collect(&service,ticket,&reply,100));
        configASSERT(reply.result==DMA_SERVICE_OK && reply.phase==DMA_SERVICE_READY);
        request.operation=DMA_SERVICE_CLOSE;
        configASSERT(dma_service_send(&service,&request,10,&ticket));
        configASSERT(dma_service_collect(&service,ticket,&reply,100));
        configASSERT(reply.result==DMA_SERVICE_OK && reply.phase==DMA_SERVICE_CLOSED);
    }
    xTaskNotifyGive(bootstrap_task);vTaskSuspend(NULL);configASSERT(0);
}
static void bootstrap(void *unused)
{
    (void)unused;configASSERT(ulTaskNotifyTake(pdTRUE,1000)==1);
    if(!retained_mode) {
        /* Authentication checks the private copied record, not its ID alone. */
        struct a733_dma_platform_config modified=bad_auth.handoff.allowed;
        ++modified.desired_cpu;
        configASSERT(bad_auth.operations.acquire(&bad_auth,&modified,bad_auth.platform.shared,
            &bad_auth.platform.token)==A733_DMA_PLATFORM_RETAINED);
        configASSERT(!bad_auth.token.id && !atomic_load(&platform_reads));
        /* Inject a counter regression before registry's internal sample. The
         * same failed provider must also reject later platform observations. */
        bad_clock.last_ticks=UINT64_MAX;
        configASSERT(bad_time.operations.acquire(&bad_time,&bad_time.handoff.allowed,bad_time.platform.shared,
            &bad_time.platform.token)==A733_DMA_PLATFORM_RETAINED);
        uint64_t untouched=0x123;
        configASSERT(bad_registry.failed && atomic_load(&bad_clock.failed));
        configASSERT(!a733_counter_clock_now(&bad_clock,&untouched) && untouched==0x123);
        configASSERT(a733_dma_startup_poll(&bad_time)==A733_DMA_PLATFORM_RETAINED && !bad_time.token.id);
    }
    if(retained_mode==2) {
        /* Observe an independently admitted token through the real registry;
         * the observer must not sample time or change ownership. */
        configASSERT(bad_time.operations.acquire(&bad_time,&bad_time.handoff.allowed,bad_time.platform.shared,
            &bad_time.platform.token)==A733_DMA_PLATFORM_READY);
        uint64_t observed=bad_registry.last_now;
        configASSERT(a733_clock_token_status(&bad_registry,&bad_time.token)==A733_CLOCK_OK);
        struct a733_clock_token altered=bad_time.token;++altered.owner;
        configASSERT(a733_clock_token_status(&bad_registry,&altered)==A733_CLOCK_INVALID);
        configASSERT(bad_registry.last_now==observed);
        atomic_store(&bad_registry.busy,1);
        configASSERT(a733_clock_token_status(&bad_registry,&bad_time.token)==A733_CLOCK_BUSY);
        configASSERT(a733_dma_startup_prepared(&bad_time,DMA,1)==0 && !bad_time.failed);
        atomic_store(&bad_registry.busy,0);
        configASSERT(a733_clock_retain(&bad_registry,&bad_time.token)==A733_CLOCK_FAULT);
        configASSERT(a733_clock_token_status(&bad_registry,&bad_time.token)==A733_CLOCK_FAULT);
        configASSERT(a733_dma_startup_prepared(&bad_time,DMA,1)==-1 && bad_time.failed);
    }
    configASSERT(dma_service_activate(&service)==DMA_SERVICE_BUSY && prepared_calls==1);
    configASSERT(!atomic_load(&service_calls) && !atomic_load(&platform_reads));
    xTaskNotifyGive(holder_task);configASSERT(ulTaskNotifyTake(pdTRUE,1000)==1);
    configASSERT(a733_dma_startup_poll(&startup)==A733_DMA_PLATFORM_PENDING);
    uint64_t id=startup.token.id;configASSERT(id);
    configASSERT(a733_dma_startup_poll(&startup)==A733_DMA_PLATFORM_PENDING);
    configASSERT(atomic_load_explicit(&holder_done,memory_order_acquire));
    enum a733_dma_platform_result result=A733_DMA_PLATFORM_PENDING;
    for(unsigned i=0;i<8 && result!=A733_DMA_PLATFORM_READY && result!=A733_DMA_PLATFORM_RETAINED;++i) {
        vTaskDelay(1);result=a733_dma_startup_poll(&startup);
    }
    configASSERT(startup.token.id==id && startup.platform.token.id==id && !atomic_load(&service_calls));
    if(retained_mode==1) {
        configASSERT(result==A733_DMA_PLATFORM_RETAINED && atomic_load(&platform_writes)==1);
        unsigned reads=atomic_load(&platform_reads);
        configASSERT(a733_dma_startup_poll(&startup)==A733_DMA_PLATFORM_RETAINED);
        configASSERT(atomic_load(&platform_reads)==reads && (ccu[0x704/4]&0x10000));
        configASSERT(dma_service_activate(&service)==DMA_SERVICE_FAULT && prepared_calls==2);
        configASSERT(dma_service_activate(&service)==DMA_SERVICE_FAULT && prepared_calls==2);
        struct a733_clock_token other={0};
        configASSERT(a733_clock_acquire(&registry,99,0,A733_CLOCK_DMA0,deadline(),&other)==A733_CLOCK_BUSY);
    } else if(retained_mode==2) {
        configASSERT(result==A733_DMA_PLATFORM_READY);
        configASSERT(a733_platform_lock_enter(&locks,A733_PLATFORM_REGISTER_LOCK,deadline()));
        configASSERT(a733_platform_lock_retain(&locks,A733_PLATFORM_REGISTER_LOCK));
        configASSERT(dma_service_activate(&service)==DMA_SERVICE_FAULT && prepared_calls==2);
        configASSERT(dma_service_activate(&service)==DMA_SERVICE_FAULT && prepared_calls==2);
        configASSERT(startup.failed && startup.token.id==id);
        /* The retained common mutex stays owned through this terminal case. */
    } else {
        configASSERT(result==A733_DMA_PLATFORM_READY && (ccu[0x704/4]&2));
        configASSERT(dma_service_activate(&service)==DMA_SERVICE_OK && prepared_calls==2);
        configASSERT(dma_service_activate(&service)==DMA_SERVICE_BUSY && prepared_calls==2);
    }
    xTaskNotifyGive(client_task);configASSERT(ulTaskNotifyTake(pdTRUE,1000)==1);
    if(retained_mode==1) platform_puts("PASS DMA deferred startup: partial native write retains token and permanently blocks activation and worker I/O\n");
    else if(retained_mode==2) platform_puts("PASS DMA activation follow-up: aliased authority snapshot, read-only token status and retained registry/register-lock refusal\n");
    else {
        configASSERT(atomic_load(&service_calls));
        platform_puts("PASS DMA deferred startup: private handoff, common physical clock, real shared mutex, cross-core activation and client admission\n");
    }
    platform_exit(0);
}
BaseType_t dma_startup_task_privileged(TaskFunction_t function,void *argument)
{return function==bootstrap || function==holder || dma_service_task_privileged(function,argument);}
void vApplicationStackOverflowHook(TaskHandle_t task,char *name)
{(void)task;platform_panic(name,0);}
static unsigned mode(void)
{
    char command[128]={0};uintptr_t parameters[2]={(uintptr_t)command,sizeof(command)};
    register uintptr_t operation __asm__("x0")=0x15;
    register uintptr_t arguments __asm__("x1")=(uintptr_t)parameters;
    __asm__ volatile("hlt #0xf000" : "+r"(operation) : "r"(arguments) : "memory");
    configASSERT(operation==0);
    for(unsigned i=0;i+8<sizeof(command);++i)
        if(!memcmp(command+i,"retained",8))return 1;
    for(unsigned i=0;i+6<sizeof(command);++i)
        if(!memcmp(command+i,"health",6))return 2;
    return 0;
}
void kernel_main(void)
{
    platform_init();retained_mode=mode();
    __asm__ volatile("mrs %0,cntfrq_el0":"=r"(frequency));
    configASSERT(frequency && frequency<=UINT32_MAX);
    configASSERT(a733_counter_clock_init(&clock_source,(uint32_t)frequency));
    configASSERT(a733_counter_clock_init(&bad_clock,(uint32_t)frequency));
    configASSERT(a733_clock_leases_init(&registry,a733_counter_clock_now,&clock_source));
    configASSERT(a733_clock_leases_init(&bad_registry,a733_counter_clock_now,&bad_clock));
    configASSERT(a733_platform_locks_init(&locks,a733_counter_clock_now,&clock_source));
    configASSERT(a733_platform_locks_init(&bad_locks,a733_counter_clock_now,&bad_clock));
    struct a733_dma_handoff handoff={
        .allowed={.fabric={0x03000002,0x82000001,0x84000001},.rate_hz={200000000,400000000,200000000},
            .desired_cpu=1,.route_change_mask=1,.owner=7,.handoff=123,.deadline_us=deadline(),.cold=1},
        .controller=DMA,.evidence=123,.established=A733_DMA_HANDOFF_REQUIRED,.owned_channels=0xffff,.service_channels=1};
    configASSERT(!a733_dma_startup_init(&startup,&handoff,&clock_source,&bad_registry,&locks));
    configASSERT(a733_dma_startup_init(&startup,&handoff,&clock_source,&registry,&locks));
    configASSERT(a733_dma_startup_init(&bad_auth,&handoff,&clock_source,&registry,&locks));
    configASSERT(a733_dma_startup_init(&bad_time,&handoff,&bad_clock,&bad_registry,&bad_locks));
    ccu[0]=0xf8000000|(99<<16)|(99<<8);
    ccu[0xa0/4]=0xfe000000|(1<<20)|(2<<16)|(99<<8)|(4<<2);
    ccu[0x1908/4]=0x8fff0fff;ccu[0x1988/4]=0x0fff0000;
    ccu[0x500/4]=handoff.allowed.fabric[0];ccu[0x580/4]=handoff.allowed.fabric[1];ccu[0x588/4]=handoff.allowed.fabric[2];
    ccu[0x5e4/4]=0x40302;
    /* Destroy the caller's authority metadata after its private copy. */
    memset(&handoff,0,sizeof(handoff));
    bootstrap_task=xTaskCreateStaticAffinitySet(bootstrap,"dma-bootstrap",3072,NULL,3,stacks[0],&task_storage[0],1);
    holder_task=xTaskCreateStaticAffinitySet(holder,"register-owner",3072,NULL,4,stacks[1],&task_storage[1],2);
    client_task=xTaskCreateStaticAffinitySet(client,"dma-client",3072,NULL,2,stacks[2],&task_storage[2],2);
    configASSERT(bootstrap_task && holder_task && client_task);
    configASSERT(a733_dma_startup_bind_task(&startup,bootstrap_task));
    configASSERT(a733_dma_startup_bind_task(&bad_auth,bootstrap_task));
    configASSERT(a733_dma_startup_bind_task(&bad_time,bootstrap_task));
    const struct a733_dma_operations ops={read_dma,write_dma,cache,barrier,ticks,mapping,fifo};
    struct a733_dma_segment segment={{(uintptr_t)pool+64,0,4,8},{(uintptr_t)pool+128,0,4,8},64};
    struct a733_dma_mapping mappings[]={{pool+64,(uintptr_t)pool+64,64,A733_DMA_READ},
        {pool+128,(uintptr_t)pool+128,64,A733_DMA_WRITE}};
    struct dma_service_job job={DMA_SERVICE_RAM,0,{{pool,(uintptr_t)pool,64,A733_DMA_DESCRIPTORS},&segment,1,mappings,2},NULL,NULL};
    struct dma_service_protected protected[]={
        {(uintptr_t)&service,(uintptr_t)&service,sizeof(service)},
        {(uintptr_t)&startup,(uintptr_t)&startup,sizeof(startup)},
        {(uintptr_t)&clock_source,(uintptr_t)&clock_source,sizeof(clock_source)},
        {(uintptr_t)&registry,(uintptr_t)&registry,sizeof(registry)},
        {(uintptr_t)&locks,(uintptr_t)&locks,sizeof(locks)}};
    struct dma_service_activation activation={bootstrap_task,prepared,&startup};
    const struct dma_service_activation *input=&activation;
    if(retained_mode==2) {
        /* The caller's metadata aliases storage initialization will overwrite. */
        struct dma_service_activation *alias=(void *)&service.jobs[0];
        memcpy(alias,&activation,sizeof(activation));input=alias;
    }
    configASSERT(dma_service_init_deferred(&service,&ops,NULL,DMA,1,frequency/100,&job,1,protected,5,5,3,input));
    configASSERT(dma_service_bind(&service,0,client_task,1));
    memset(&activation,0,sizeof(activation));
    vTaskStartScheduler();platform_panic("scheduler returned",0);
}
