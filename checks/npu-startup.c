/* The NPU startup sequencer on two QEMU cores with real FreeRTOS tasks and
 * mutexes, the real counter clock, clock registry, rail ledger and register
 * lock. A modeled GPU/PCK manager reads fixed words. The power-domain owner is
 * modeled by a service task on core 1 that publishes the authority bits and
 * answers the domain-4 row; the host model checks/npu-startup-host.c runs the
 * actual owner. Modes: success (startup to service activation, and a client
 * on another core that cannot step the startup); "readback" (a completed
 * write that reads back wrong retains the register lock and every token, and
 * the service stays inactive). */
#include "a733_npu_startup.h"
#include "npu_service.h"
#include "platform.h"
#include <string.h>

#define CCU 0x02002000u
#define PCK 0x07064000u
static struct a733_counter_clock clock_source;
static struct a733_clock_leases registry;
static struct a733_platform_locks locks;
static struct a733_gpu_resource_manager manager;
static struct a733_rail_leases rails;
static struct a733_pmic pmic;
static struct a733_power_domains domains;
static struct a733_npu_startup startup;
static struct npu_service service;
static StaticTask_t task_storage[3];
static StackType_t stacks[3][3072];
static TaskHandle_t bootstrap_task,client_task,service_task;
static uint32_t ccu[0x2000/4],pck[2],sysctrl=2; /* The boot ROM's CPU routing. */
static uint64_t counter_mhz;
static atomic_uint reads,writes,backend_calls,notified;
static unsigned failure_mode,command_token,prepared_calls;
static uint64_t deadline(void)
{uint64_t now;configASSERT(a733_counter_clock_now(&clock_source,&now));return now+1000000;}
static uint32_t *reg(uintptr_t a){configASSERT(a>=CCU && a<CCU+sizeof(ccu));return &ccu[(a-CCU)/4];}
/* NPU register model: only the bootstrap task, and writes only under the
 * register lock. PLL_NPU locks as soon as its enable table completes. */
static int npu_read(void *v,uintptr_t a,uint32_t *out)
{
    configASSERT(v==&command_token && xTaskGetCurrentTaskHandle()==bootstrap_task);
    atomic_fetch_add(&reads,1);
    if(a==0x07090160)*out=0x10000002;
    else if(a==0x03000008)*out=sysctrl;
    else if(a==PCK || a==PCK+8)*out=pck[a==PCK+8];
    else {
        *out=*reg(a);
        if(a==CCU+0x2a0 && (*out&0xa0000000)==0xa0000000)*out|=1u<<28;
        if(a==CCU+0x19a0)*out=(ccu[0x1920/4]&0x10001)==0x10001 ? 1u<<16 : 0;
        if(failure_mode && a==CCU+0xb04 && atomic_load(&writes)==9)*out^=1u<<17;
    }
    return 1;
}
static int npu_write(void *v,uintptr_t a,uint32_t value)
{
    configASSERT(v==&command_token && xTaskGetCurrentTaskHandle()==bootstrap_task);
    configASSERT(xSemaphoreGetMutexHolder(locks.mutex[A733_PLATFORM_REGISTER_LOCK])==bootstrap_task);
    configASSERT(a==CCU+0xb00 || a==CCU+0xb04 || a==CCU+0x1920 || a==CCU+0x2a0 || a==0x03000008);
    atomic_fetch_add(&writes,1);
    if(a==0x03000008){configASSERT(!((sysctrl^value)&~2u));sysctrl=value;return 1;}
    *reg(a)=value;return 1;
}
static int npu_now(void *v,uint64_t *out){configASSERT(v==&command_token);return a733_counter_clock_now(&clock_source,out);}
static uint64_t npu_spin(void *v)
{configASSERT(v==&command_token);uint64_t t;__asm__ volatile("isb\n mrs %0,cntpct_el0":"=r"(t));return t/counter_mhz;}
static int npu_enter(void *v,uint64_t d)
{configASSERT(v==&command_token);return a733_platform_lock_enter(&locks,A733_PLATFORM_REGISTER_LOCK,d);}
static void npu_leave(void *v)
{configASSERT(v==&command_token);a733_platform_lock_leave(&locks,A733_PLATFORM_REGISTER_LOCK);}
static void npu_retain(void *v)
{configASSERT(v==&command_token);(void)a733_platform_lock_retain(&locks,A733_PLATFORM_REGISTER_LOCK);}
/* The owner's reads for a733_power_domain_check(domains,4): MBUS gates, the
 * PPU clock, then the domain-4 policy and status. */
static int owner_read(void *v,uintptr_t a,uint32_t *out)
{
    configASSERT(v==&domains);
    if(a==CCU+0x5c0)*out=1u<<6;else if(a==CCU+0x5e0)*out=1u<<18;else if(a==0x070101ac)*out=1;
    else if(a==PCK || a==PCK+8)*out=pck[a==PCK+8];else configASSERT(0);
    return 1;
}
static int owner_write(void *v,uintptr_t a,uint32_t value){(void)v;(void)a;(void)value;configASSERT(0);return 0;}
static void owner_notify(void *v){configASSERT(v==&domains);atomic_store(&notified,1);xTaskNotifyGive(service_task);}
static int manager_now(void *v,uint64_t *out){(void)v;return a733_counter_clock_now(&clock_source,out);}
static int manager_read(void *v,uintptr_t a,uint32_t *out)
{(void)v;*out=a==0x070101ac ? 1 : a==0x07090160 ? 0x10000002 : *reg(a);return 1;}
static int manager_update(void *v,uintptr_t a,uint32_t q,uint32_t s,uint64_t d)
{(void)v;(void)a;(void)q;(void)s;(void)d;configASSERT(0);return 0;}
static int manager_rail(void *v,struct a733_gpu_resource_rail *o,uint64_t d)
{(void)v;(void)o;(void)d;configASSERT(0);return 0;}
/* Modeled owner: after two ticks the supply is verified; each row request is
 * answered ON once domain 4's policy is set, as the PPU would acknowledge. */
static void service_loop(void *unused)
{
    (void)unused;vTaskDelay(2);
    a733_power_authority_set(&rails.authority,A733_POWER_SUPPLY_DONE|A733_POWER_DCDC2_VERIFIED);
    for(;;) {
        (void)ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
        const unsigned q=atomic_load_explicit(&startup.row.request,memory_order_acquire);
        if(q&1){pck[0]=8;pck[1]=8;}
        atomic_store_explicit(&startup.row.state,((q>>1)<<3)|A733_POWER_ROW_ON,memory_order_release);
    }
}
static int device_read(void *c,unsigned o,uint32_t *v){(void)c;(void)o;(void)v;configASSERT(0);return 0;}
static int device_write(void *c,unsigned o,uint32_t v){(void)c;(void)o;(void)v;configASSERT(0);return 0;}
static uint64_t microseconds(void *c)
{
    configASSERT(c==&command_token && startup.stage==A733_NPU_STARTUP_READY && !startup.failed && xTaskGetCurrentTaskHandle()==service.worker);
    atomic_fetch_add(&backend_calls,1);uint64_t now;configASSERT(a733_counter_clock_now(&clock_source,&now));return now;
}
static void delay(void *c,unsigned us){(void)c;(void)us;configASSERT(0);}
static void barrier(void *c){(void)c;configASSERT(0);}
static int publish(void *c,const struct a733_npu_mmu_image *i,const struct a733_npu_command *m,int b)
{(void)c;(void)i;(void)m;(void)b;configASSERT(0);return 0;}
static int finish(void *c,const struct a733_npu_command *m,int x){(void)c;(void)m;(void)x;configASSERT(0);return 0;}
static int prepared(void *cookie,const struct a733_npu_ops *o,void *b,const struct a733_npu_mmu_image *i,
    const struct a733_npu_command *c,unsigned n)
{(void)o;(void)b;(void)i;(void)c;(void)n;++prepared_calls;return a733_npu_startup_prepared(cookie);}
static void client(void *unused)
{
    (void)unused;
    /* Another task cannot step the startup: no register access, no change. */
    configASSERT(a733_npu_startup_step(&startup)==A733_NPU_PLATFORM_RETAINED && !startup.failed);
    configASSERT(!atomic_load(&reads) && npu_service_activate(&service)==NPU_SERVICE_DENIED);
    xTaskNotifyGive(bootstrap_task);vTaskSuspend(NULL);configASSERT(0);
}
static void bootstrap(void *unused)
{
    (void)unused;configASSERT(ulTaskNotifyTake(pdTRUE,1000)==1);
    configASSERT(npu_service_activate(&service)==NPU_SERVICE_BUSY && prepared_calls==1 && !startup.failed);
    const TickType_t first=xTaskGetTickCount();unsigned calls=0;
    enum a733_npu_platform_result r=A733_NPU_PLATFORM_PENDING;
    while(r==A733_NPU_PLATFORM_PENDING && calls<100){r=a733_npu_startup_step(&startup);++calls;if(r==A733_NPU_PLATFORM_PENDING)vTaskDelay(1);}
    if(failure_mode) {
        configASSERT(r==A733_NPU_PLATFORM_RETAINED && startup.failed && startup.register_held);
        configASSERT(atomic_load(&locks.retained) && xSemaphoreGetMutexHolder(locks.mutex[A733_PLATFORM_REGISTER_LOCK])==bootstrap_task);
        configASSERT(a733_clock_token_status(&registry,&startup.clock)==A733_CLOCK_OK && a733_rail_status(&rails,&startup.rail)==A733_RAIL_READY);
        configASSERT(!a733_npu_startup_window_open(&startup));
        const unsigned before=atomic_load(&reads);
        configASSERT(a733_npu_startup_step(&startup)==A733_NPU_PLATFORM_RETAINED && before==atomic_load(&reads));
        configASSERT(npu_service_activate(&service)==NPU_SERVICE_FAULT && !atomic_load(&backend_calls));
        platform_puts("PASS NPU startup readback mismatch retains the register lock and every token; the service stays inactive\n");
        platform_exit(0);
    }
    configASSERT(r==A733_NPU_PLATFORM_READY && atomic_load(&notified) && pck[0]==8 && ccu[0xb00/4]==0x80000000);
    configASSERT(!sysctrl && a733_npu_startup_window_open(&startup)); /* The SRAM is routed to the NPU. */
    platform_puts("COST calls=");platform_hex(calls);platform_puts(" ticks=");platform_hex(xTaskGetTickCount()-first);platform_puts("\n");
    configASSERT(npu_service_activate(&service)==NPU_SERVICE_OK);
    for(unsigned i=0;i<100 && !atomic_load(&backend_calls);++i)vTaskDelay(1);
    configASSERT(atomic_load(&backend_calls)); /* The activated worker ran. */
    platform_puts("PASS NPU startup: one sequencer, real locks and leases, row answered across cores, service activation\n");
    platform_exit(0);
}
BaseType_t npu_startup_task_privileged(TaskFunction_t entry,void *argument)
{return entry==bootstrap || npu_service_task_privileged(entry,argument);}
void vApplicationStackOverflowHook(TaskHandle_t task,char *name){(void)task;platform_panic(name,0);}
static unsigned mode(void)
{
    char command[128]={0};uintptr_t parameters[2]={(uintptr_t)command,sizeof(command)};
    register uintptr_t operation __asm__("x0")=0x15;
    register uintptr_t arguments __asm__("x1")=(uintptr_t)parameters;
    __asm__ volatile("hlt #0xf000" : "+r"(operation) : "r"(arguments) : "memory");
    configASSERT(operation==0);
    for(unsigned i=0;i+8<sizeof(command);++i)if(!memcmp(command+i,"readback",8))return 1;
    return 0;
}
void kernel_main(void)
{
    platform_init();failure_mode=mode();uint64_t frequency;
    __asm__ volatile("mrs %0,cntfrq_el0":"=r"(frequency));counter_mhz=frequency/1000000;
    configASSERT(a733_counter_clock_init(&clock_source,(uint32_t)frequency));
    configASSERT(a733_clock_leases_init(&registry,a733_counter_clock_now,&clock_source));
    configASSERT(a733_platform_locks_init(&locks,a733_counter_clock_now,&clock_source));
    const struct a733_gpu_resource_ops mops={manager_now,manager_read,manager_update,manager_rail};
    configASSERT(a733_gpu_resources_init(&manager,&mops,NULL));
    const struct a733_rail_binding binding={&registry,&manager,900,a733_counter_clock_now,&clock_source};
    configASSERT(a733_rail_leases_init(&rails,&binding));
    pmic.config.startup_deadline=deadline();
    domains=(struct a733_power_domains){.self=&domains,.pmic=&pmic,.ops={owner_read,owner_write,owner_notify},.context=&domains};
    ccu[0]=0xf8000000|(99<<16)|(99<<8);
    ccu[0xa0/4]=0xfe000000|(1<<20)|(2<<16)|(99<<8)|(4<<2);
    ccu[0x1908/4]=0x0fff0fff;ccu[0x1988/4]=0x0fff0000;
    ccu[0x500/4]=0x03000001;ccu[0x580/4]=0x82000001;ccu[0x588/4]=0x84000000;
    ccu[0xb04/4]=1u<<19;ccu[0xc00/4]=1u<<31;ccu[0xc0c/4]=1u<<16;ccu[0x2ac/4]=0x30000000;ccu[0x2b0/4]=0x000b0000;
    const struct a733_npu_startup_config c={{{0x03000001,0x82000001,0x84000000},{300000000,400000000,400000000}},7,deadline(),4};
    const struct a733_npu_startup_providers p={&clock_source,&registry,&locks,&manager,&rails,&domains};
    const struct a733_npu_startup_ops o={{npu_read,npu_write},npu_now,npu_spin,npu_enter,npu_leave,npu_retain,NULL};
    configASSERT(a733_npu_startup_init(&startup,&c,&p,&o,&command_token));
    bootstrap_task=xTaskCreateStaticAffinitySet(bootstrap,"npu-bootstrap",3072,NULL,4,stacks[0],&task_storage[0],1);
    service_task=xTaskCreateStaticAffinitySet(service_loop,"power",3072,NULL,3,stacks[1],&task_storage[1],2);
    client_task=xTaskCreateStaticAffinitySet(client,"npu-client",3072,NULL,2,stacks[2],&task_storage[2],2);
    configASSERT(bootstrap_task && service_task && client_task && a733_npu_startup_bind_task(&startup,bootstrap_task));
    static const struct a733_npu_ops backend={device_read,device_write,microseconds,delay,barrier,publish,finish};
    static const struct a733_npu_mmu_image image={0x44000000,0x5000,0x44000000,1};
    static const struct a733_npu_command commands[1]={{0x100000,A733_NPU_FETCH_BYTES,&command_token}};
    const struct npu_service_activation activation={bootstrap_task,prepared,&startup};
    configASSERT(npu_service_init_deferred(&service,&backend,&command_token,&image,commands,1,5,3,&activation));
    configASSERT(npu_service_bind(&service,0,client_task,0));
    vTaskStartScheduler();platform_panic("scheduler returned",0);
}
