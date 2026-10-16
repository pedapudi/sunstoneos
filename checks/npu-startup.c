#include "a733_npu_startup.h"
#include "platform.h"
#include <string.h>

#define CCU 0x02002000u
#define PCK 0x07064000u
static struct a733_counter_clock clock_source,other_clock;
static struct a733_clock_leases registry,other_registry;
static struct a733_platform_locks locks;
static struct a733_npu_startup_resources resources;
static struct a733_npu_startup startup,bad_auth;
static struct npu_service service;
static StaticTask_t task_storage[3];
static StackType_t stacks[3][3072];
static TaskHandle_t bootstrap_task,client_task,holder_task;
static uint32_t ccu[0x2000/4],pck[0x1000/4];
static atomic_uint platform_reads,platform_writes,backend_calls,holder_done;
static unsigned failure_mode,prepared_calls,rail_reads,closes,reenter;
static unsigned command_token, injected;
void __real_a733_platform_lock_leave(struct a733_platform_locks *,enum a733_platform_lock);
void __wrap_a733_platform_lock_leave(struct a733_platform_locks *p,enum a733_platform_lock lock)
{
    __real_a733_platform_lock_leave(p,lock);
    if(failure_mode==4 && !injected && lock==A733_PLATFORM_REGISTER_LOCK && startup.platform.stage==5) {
        uint64_t now;injected=1;
        do {configASSERT(a733_counter_clock_now(&clock_source,&now));} while(now<startup.handoff.allowed.deadline_us);
    }
}
static uint64_t first_snapshot_time,second_snapshot_time;
static uint64_t deadline(void)
{uint64_t now;configASSERT(a733_counter_clock_now(&clock_source,&now));return now+1000000;}
static uint32_t *reg(uintptr_t address)
{
    if(address>=CCU && address<CCU+sizeof(ccu))return &ccu[(address-CCU)/4];
    configASSERT(address>=PCK && address<PCK+sizeof(pck));return &pck[(address-PCK)/4];
}
int __wrap_a733_npu_platform_native_read(void *cookie,uintptr_t address,uint32_t *out)
{
    (void)cookie;
    configASSERT(xTaskGetCurrentTaskHandle()==bootstrap_task);
    configASSERT(xSemaphoreGetMutexHolder(locks.mutex[0])==bootstrap_task);
    atomic_fetch_add(&platform_reads,1);
    if(address==0x07090160)*out=0;
    else if(address==0x070101ac)*out=1;
    else if(address==PCK+8 && (pck[0]&15)==8)*out=8;
    else *out=*reg(address);
    if(failure_mode==2 && !injected && address==CCU+0xb04 && atomic_load(&platform_writes)) {
        *out^=1u<<18;injected=1; /* Successful transfer, incorrect readback. */
    } else if(failure_mode==3 && !injected && startup.platform.stage==4 && address==CCU+0xb00) {
        uint64_t now;injected=1;
        do {configASSERT(a733_counter_clock_now(&clock_source,&now));} while(now<startup.handoff.allowed.deadline_us);
    }
    return 1;
}
int __wrap_a733_npu_platform_native_write(void *cookie,uintptr_t address,uint32_t value)
{
    (void)cookie;configASSERT(xTaskGetCurrentTaskHandle()==bootstrap_task);
    configASSERT(xSemaphoreGetMutexHolder(locks.mutex[0])==bootstrap_task);
    atomic_fetch_add(&platform_writes,1);*reg(address)=value;return 1;
}
static int inherited_read(void *cookie,uintptr_t address,uint32_t *out)
{
    configASSERT(cookie==&resources.native && xTaskGetCurrentTaskHandle()==bootstrap_task);
    if(address==0x0708300c || address==0x07083010 || address==0x07083020) {
        configASSERT(xSemaphoreGetMutexHolder(locks.mutex[1])==bootstrap_task);
        *out=address==0x0708300c ? 0x40 : address==0x07083010 ? 0xf8 : 0x30;
    } else if(address==0x07090160)*out=0;
    else if(address==0x070101ac)*out=1;
    else *out=*reg(address);
    return 1;
}
static int inherited_update(void *cookie,uintptr_t a,uint32_t required,uint32_t set,uint64_t d)
{(void)cookie;(void)a;(void)required;(void)set;(void)d;configASSERT(0);return 0;}
static int inherited_rail(void *cookie,struct a733_gpu_resource_rail *out,uint64_t d)
{(void)cookie;(void)out;(void)d;configASSERT(0);return 0;}
static int inherited_now(void *cookie,uint64_t *out)
{(void)cookie;return a733_counter_clock_now(&clock_source,out);}
int __wrap_a733_gpu_resources_native_init(struct a733_gpu_resource_native *n,const struct a733_gpu_resource_locks *l)
{
    configASSERT(n==&resources.native && l==&locks.callbacks);
    n->locks=*l;n->operations=(struct a733_gpu_resource_ops){inherited_now,inherited_read,inherited_update,inherited_rail};
    n->initialized=1;return 1;
}
enum i2c_result __wrap_a733_i2c_open(struct a733_i2c *bus)
{
    configASSERT(bus==&startup.bus && xTaskGetCurrentTaskHandle()==bootstrap_task);
    configASSERT(xSemaphoreGetMutexHolder(locks.mutex[1])==bootstrap_task && atomic_load(&holder_done));
    bus->control=0x40;bus->opened=1;return I2C_OK;
}
enum i2c_result __wrap_a733_i2c_read8(struct a733_i2c *bus,uint8_t address,uint8_t index,uint8_t *out)
{
    static const uint8_t sequence[]={0xff,0x0e,0x0f,0x10,0x13,0xff};
    static const uint8_t values[]={0,3,0,0x0a,30,0};
    configASSERT(bus==&startup.bus && bus->opened && address==0x36 && index==sequence[rail_reads%6]);
    configASSERT(xSemaphoreGetMutexHolder(locks.mutex[1])==bootstrap_task);
    unsigned slot=rail_reads++%6;
    if(!slot) {
        uint64_t now;configASSERT(a733_counter_clock_now(&clock_source,&now));
        if(rail_reads==1)first_snapshot_time=now;else second_snapshot_time=now;
    }
    *out=failure_mode==1 && slot==4 ? 31 : values[slot];return I2C_OK;
}
void __wrap_a733_i2c_close(struct a733_i2c *bus)
{configASSERT(bus==&startup.bus && bus->opened);bus->opened=0;++closes;}
static int device_read(void *cookie,unsigned offset,uint32_t *out)
{(void)cookie;(void)offset;(void)out;configASSERT(0);return 0;}
static int device_write(void *cookie,unsigned offset,uint32_t value)
{(void)cookie;(void)offset;(void)value;configASSERT(0);return 0;}
static uint64_t microseconds(void *cookie)
{
    configASSERT(cookie==&command_token && a733_npu_startup_ready(&startup));
    configASSERT(xTaskGetCurrentTaskHandle()==service.worker);
    atomic_fetch_add(&backend_calls,1);uint64_t now;
    configASSERT(a733_counter_clock_now(&clock_source,&now));return now;
}
static void delay(void *cookie,unsigned us){(void)cookie;(void)us;configASSERT(0);}
static void barrier(void *cookie){(void)cookie;configASSERT(0);}
static int publish(void *cookie,const struct a733_npu_mmu_image *image,const struct a733_npu_command *command,int bootstrap)
{(void)cookie;(void)image;(void)command;(void)bootstrap;configASSERT(0);return 0;}
static int finish(void *cookie,const struct a733_npu_command *command,int canceled)
{(void)cookie;(void)command;(void)canceled;configASSERT(0);return 0;}
static int prepared(void *cookie,const struct a733_npu_ops *ops,void *backend,
    const struct a733_npu_mmu_image *image,const struct a733_npu_command *commands,unsigned count)
{
    ++prepared_calls;
    if(reenter) {
        reenter=0;unsigned before=prepared_calls;
        configASSERT(npu_service_activate(&service)==NPU_SERVICE_BUSY && prepared_calls==before);
    }
    return a733_npu_startup_prepared(cookie,ops,backend,image,commands,count);
}
static void holder(void *unused)
{
    (void)unused;configASSERT(ulTaskNotifyTake(pdTRUE,1000)==1);
    configASSERT(a733_platform_lock_enter(&locks,A733_PLATFORM_I2C_LOCK,deadline()));
    xTaskNotifyGive(bootstrap_task);vTaskDelay(3);
    atomic_store(&holder_done,1);a733_platform_lock_leave(&locks,A733_PLATFORM_I2C_LOCK);
    vTaskSuspend(NULL);configASSERT(0);
}
static void client(void *unused)
{
    (void)unused;
    struct npu_service_request request={NPU_SERVICE_STATUS,0,0};uint64_t ticket=0xabc;
    configASSERT(!npu_service_send(&service,&request,0,&ticket) && ticket==0xabc);
    if(failure_mode<2)configASSERT(npu_service_activate(&service)==NPU_SERVICE_DENIED);
    configASSERT(a733_npu_startup_poll(&startup)==A733_NPU_PLATFORM_RETAINED && !startup.failed);
    configASSERT(!prepared_calls && !atomic_load(&backend_calls) && !atomic_load(&platform_reads));
    xTaskNotifyGive(bootstrap_task);configASSERT(ulTaskNotifyTake(pdTRUE,1000)==1);
    if(failure_mode) {
        configASSERT(!npu_service_send(&service,&request,0,&ticket) && ticket==0xabc);
        configASSERT(!atomic_load(&backend_calls));
    } else {
        configASSERT(npu_service_send(&service,&request,10,&ticket));
        struct npu_service_reply reply;
        configASSERT(npu_service_collect(&service,ticket,&reply,100));
        configASSERT(reply.result==NPU_SERVICE_OK && reply.state==A733_NPU_COLD && !reply.owns_session);
    }
    xTaskNotifyGive(bootstrap_task);vTaskSuspend(NULL);configASSERT(0);
}
static void register_failure(void)
{
    /* Only the changed register-lock failure boundary; existing rail, success
     * and client-authority evidence is reused. No device command executes. */
    atomic_store(&holder_done,1);
    enum a733_npu_platform_result result=A733_NPU_PLATFORM_PENDING;
    for(unsigned i=0;i<150 && result!=A733_NPU_PLATFORM_RETAINED;++i) {
        result=a733_npu_startup_poll(&startup);
        configASSERT(result!=A733_NPU_PLATFORM_READY);
        if(result!=A733_NPU_PLATFORM_RETAINED)vTaskDelay(1);
    }
    configASSERT(injected && result==A733_NPU_PLATFORM_RETAINED);
    configASSERT(rail_reads==12 && closes==2 && !startup.i2c_held);
    configASSERT(startup.failed && startup.platform.failed);
    if(failure_mode==4) {
        configASSERT(!startup.register_held && !atomic_load(&locks.retained));
        configASSERT(!xSemaphoreGetMutexHolder(locks.mutex[0]));
    } else {
        configASSERT(startup.register_held && atomic_load(&locks.retained)==1);
        configASSERT(xSemaphoreGetMutexHolder(locks.mutex[0])==bootstrap_task);
    }
    configASSERT(startup.clock_token.id && startup.rail_token.id && startup.pck_token.token);
    unsigned reads=atomic_load(&platform_reads),writes=atomic_load(&platform_writes);
    configASSERT(reads && writes && a733_npu_startup_poll(&startup)==A733_NPU_PLATFORM_RETAINED);
    configASSERT(npu_service_activate(&service)==NPU_SERVICE_FAULT);
    configASSERT(reads==atomic_load(&platform_reads) && writes==atomic_load(&platform_writes));
    configASSERT(!atomic_load(&backend_calls));
    xTaskNotifyGive(client_task);configASSERT(ulTaskNotifyTake(pdTRUE,1000)==1);
    platform_puts(failure_mode==2 ?
        "PASS NPU register readback mismatch retains shared mutex and leases; inactive service remains silent\n" :
        failure_mode==3 ? "PASS NPU late final read retains shared mutex and leases; inactive service remains silent\n" :
        "PASS NPU late successful unlock faults with leases retained and mutex released; inactive service remains silent\n");
    platform_exit(0);
}
static void bootstrap(void *unused)
{
    (void)unused;configASSERT(ulTaskNotifyTake(pdTRUE,1000)==1);
    if(failure_mode>=2){register_failure();configASSERT(0);}
    struct a733_npu_command changed=startup.handoff.commands[0];++changed.readable_bytes;
    configASSERT(a733_npu_startup_prepared(&bad_auth,&startup.handoff.backend,startup.handoff.backend_cookie,
        &startup.handoff.image,&changed,1)==-1 && bad_auth.failed);
    configASSERT(!bad_auth.clock_token.id && !atomic_load(&platform_reads));
    configASSERT(npu_service_activate(&service)==NPU_SERVICE_BUSY && prepared_calls==1);
    for(unsigned i=0;i<3;++i)configASSERT(a733_npu_startup_poll(&startup)==A733_NPU_PLATFORM_PENDING);
    configASSERT(startup.clock_token.id && startup.rail_token.id && startup.pck_token.token && !rail_reads);
    struct a733_clock_token other={0};uint64_t d=deadline();
    configASSERT(a733_clock_acquire(&registry,99,0,A733_POWER_DCDC2,d,&other)==A733_CLOCK_BUSY);
    uint64_t token=0;
    configASSERT(a733_gpu_resources_change(&resources.manager,12,A733_GPU_RESOURCE_PCK,&token)==A733_GPU_RESOURCE_BUSY);
    configASSERT(a733_gpu_resources_change(&resources.manager,12,A733_GPU_RESOURCE_PARENT,&token)==A733_GPU_RESOURCE_BUSY);
    uint64_t prior=resources.manager.last_now;
    configASSERT(a733_gpu_resources_status(&resources.manager,0,&startup.pck_token)==A733_GPU_RESOURCE_OK);
    struct a733_gpu_resource_lease altered=startup.pck_token;altered.mask|=A733_GPU_RESOURCE_RAIL;
    configASSERT(a733_gpu_resources_status(&resources.manager,0,&altered)==A733_GPU_RESOURCE_INVALID);
    configASSERT(resources.manager.last_now==prior);
    xTaskNotifyGive(holder_task);configASSERT(ulTaskNotifyTake(pdTRUE,1000)==1);
    enum a733_npu_platform_result result=a733_npu_startup_poll(&startup);
    for(unsigned i=0;i<60 && result!=A733_NPU_PLATFORM_READY && result!=A733_NPU_PLATFORM_RETAINED;++i) {
        vTaskDelay(1);result=a733_npu_startup_poll(&startup);
    }
    configASSERT(!atomic_load(&backend_calls));
    if(failure_mode) {
        configASSERT(result==A733_NPU_PLATFORM_RETAINED && rail_reads==5 && !closes);
        configASSERT(startup.i2c_held && atomic_load(&locks.retained)==2);
        configASSERT(xSemaphoreGetMutexHolder(locks.mutex[1])==bootstrap_task);
        configASSERT(!atomic_load(&platform_reads) && !atomic_load(&platform_writes));
        unsigned before=rail_reads;configASSERT(a733_npu_startup_poll(&startup)==A733_NPU_PLATFORM_RETAINED);
        configASSERT(rail_reads==before && npu_service_activate(&service)==NPU_SERVICE_FAULT);
        unsigned calls=prepared_calls;configASSERT(npu_service_activate(&service)==NPU_SERVICE_FAULT && prepared_calls==calls);
        configASSERT(a733_clock_token_status(&registry,&startup.clock_token)==A733_CLOCK_OK);
        configASSERT(a733_clock_token_status(&registry,&startup.rail_token)==A733_CLOCK_OK);
        configASSERT(a733_gpu_resources_status(&resources.manager,0,&startup.pck_token)==A733_GPU_RESOURCE_OK);
    } else {
        configASSERT(result==A733_NPU_PLATFORM_READY && rail_reads==12 && closes==2);
        configASSERT(second_snapshot_time-first_snapshot_time>=A733_NPU_RAIL_SETTLE_US);
        configASSERT(atomic_load(&platform_reads) && atomic_load(&platform_writes));
        atomic_store(&resources.manager.busy,1);
        configASSERT(npu_service_activate(&service)==NPU_SERVICE_BUSY && !startup.failed);
        atomic_store(&resources.manager.busy,0);
        atomic_store(&registry.busy,1);
        configASSERT(npu_service_activate(&service)==NPU_SERVICE_BUSY && !startup.failed);
        atomic_store(&registry.busy,0);reenter=1;
        configASSERT(npu_service_activate(&service)==NPU_SERVICE_OK);
        unsigned calls=prepared_calls;configASSERT(npu_service_activate(&service)==NPU_SERVICE_BUSY && calls==prepared_calls);
    }
    xTaskNotifyGive(client_task);configASSERT(ulTaskNotifyTake(pdTRUE,1000)==1);
    if(failure_mode)platform_puts("PASS NPU retained startup: invalid DCDC2 snapshot retains all leases and PMIC mutex; service remains silent\n");
    else {
        configASSERT(atomic_load(&backend_calls));
        platform_puts("PASS NPU startup: actual lease managers and mutexes, DCDC2 snapshots/settling, private command binding and cross-core activation\n");
    }
    platform_exit(0);
}
BaseType_t npu_startup_task_privileged(TaskFunction_t entry,void *argument)
{return entry==bootstrap || entry==holder || npu_service_task_privileged(entry,argument);}
void vApplicationStackOverflowHook(TaskHandle_t task,char *name)
{(void)task;platform_panic(name,0);}
static unsigned mode(void)
{
    char command[128]={0};uintptr_t parameters[2]={(uintptr_t)command,sizeof(command)};
    register uintptr_t operation __asm__("x0")=0x15;
    register uintptr_t arguments __asm__("x1")=(uintptr_t)parameters;
    __asm__ volatile("hlt #0xf000" : "+r"(operation) : "r"(arguments) : "memory");
    configASSERT(operation==0);
    for(unsigned i=0;i+11<sizeof(command);++i) {
        if(!memcmp(command+i,"readback",8))return 2;
        if(!memcmp(command+i,"late-read",9))return 3;
        if(!memcmp(command+i,"late-unlock",11))return 4;
        if(!memcmp(command+i,"retained",8))return 1;
    }
    return 0;
}
void kernel_main(void)
{
    platform_init();failure_mode=mode();uint64_t frequency;
    __asm__ volatile("mrs %0,cntfrq_el0":"=r"(frequency));
    configASSERT(a733_counter_clock_init(&clock_source,(uint32_t)frequency));
    configASSERT(a733_counter_clock_init(&other_clock,(uint32_t)frequency));
    configASSERT(a733_clock_leases_init(&registry,a733_counter_clock_now,&clock_source));
    configASSERT(a733_clock_leases_init(&other_registry,a733_counter_clock_now,&other_clock));
    configASSERT(a733_platform_locks_init(&locks,a733_counter_clock_now,&clock_source));
    configASSERT(a733_npu_startup_resources_init(&resources,&clock_source,&locks));
    struct a733_npu_handoff h={
        .allowed={.fabric={0x03000001,0x82000001,0x84000000},.rate_hz={300000000,400000000,400000000},
            .owner=7,.handoff=123,.deadline_us=deadline()},
        .backend={device_read,device_write,microseconds,delay,barrier,publish,finish},.backend_cookie=&command_token,
        .image={0x44000000,0x5000,0x44000000,1},.commands={{0x100000,A733_NPU_FETCH_BYTES,&command_token}},
        .controller=0x03600000,.evidence=123,.command_admission=234,.established=A733_NPU_HANDOFF_REQUIRED,.command_count=1};
    if(failure_mode>=2)h.allowed.deadline_us-=900000;
    configASSERT(!a733_npu_startup_init(&startup,&h,&resources,&other_registry));
    configASSERT(a733_npu_startup_init(&startup,&h,&resources,&registry));
    configASSERT(a733_npu_startup_init(&bad_auth,&h,&resources,&registry));
    ccu[0]=0xf8000000|(99<<16)|(99<<8);
    ccu[0xa0/4]=0xfe000000|(1<<20)|(2<<16)|(99<<8)|(4<<2);
    ccu[0x2a0/4]=0xf8000000|(1<<20)|(81<<8);
    ccu[0x500/4]=h.allowed.fabric[0];ccu[0x580/4]=h.allowed.fabric[1];ccu[0x588/4]=h.allowed.fabric[2];
    ccu[0x5c0/4]=0x010000ff;ccu[0x5e0/4]=0x41055800;ccu[0xb04/4]=0x80000;
    bootstrap_task=xTaskCreateStaticAffinitySet(bootstrap,"npu-bootstrap",3072,NULL,3,stacks[0],&task_storage[0],1);
    holder_task=xTaskCreateStaticAffinitySet(holder,"pmic-owner",3072,NULL,4,stacks[1],&task_storage[1],2);
    client_task=xTaskCreateStaticAffinitySet(client,"npu-client",3072,NULL,2,stacks[2],&task_storage[2],2);
    configASSERT(bootstrap_task && holder_task && client_task);
    configASSERT(a733_npu_startup_bind_task(&startup,bootstrap_task));
    configASSERT(a733_npu_startup_bind_task(&bad_auth,bootstrap_task));
    struct npu_service_activation activation={bootstrap_task,prepared,&startup};
    struct npu_service_activation *alias=(void *)&service.commands[8];memcpy(alias,&activation,sizeof(activation));
    configASSERT(npu_service_init_deferred(&service,&h.backend,h.backend_cookie,&h.image,h.commands,1,5,3,alias));
    configASSERT(npu_service_bind(&service,0,client_task,0));
    memset(&h,0,sizeof(h));memset(&activation,0,sizeof(activation));
    vTaskStartScheduler();platform_panic("scheduler returned",0);
}
