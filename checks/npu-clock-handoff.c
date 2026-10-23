/* Reuse the existing modeled MMIO/rail wrappers, without executing its old
 * scenarios. The new entry points exercise only PLL publication/adoption. */
#define kernel_main inherited_startup_kernel_main
#define npu_startup_task_privileged inherited_startup_task_privileged
#include "npu-startup.c"
#undef kernel_main
#undef npu_startup_task_privileged

static struct a733_npu_pll pll, copied_pll;
static struct a733_npu_pll_config pll_authority;
static unsigned handoff_mode,pll_reads,pll_writes,publication_count;
static struct a733_clock_token previous;
static unsigned cost_pll,cost_startup;static TickType_t cost_first;
enum a733_clock_result __real_a733_clock_publish_stable(struct a733_clock_leases *,
    const struct a733_clock_token *,a733_clock_mask_t,uint64_t,struct a733_clock_token *);
enum a733_clock_result __wrap_a733_clock_publish_stable(struct a733_clock_leases *r,
    const struct a733_clock_token *old,a733_clock_mask_t parents,uint64_t d,struct a733_clock_token *out)
{
    enum a733_clock_result result=__real_a733_clock_publish_stable(r,old,parents,d,out);
    if(result==A733_CLOCK_OK) {
        ++publication_count;
        if(handoff_mode==2) {
            uint64_t t;
            do {configASSERT(a733_counter_clock_now(&clock_source,&t));} while(t<d);
        }
    }
    return result;
}
static enum a733_npu_pll_result pll_acquire(void *cookie,const struct a733_npu_pll_config *c,
    a733_clock_mask_t shared,a733_clock_mask_t exclusive,struct a733_npu_pll_resources *out)
{
    configASSERT(cookie==&pll_authority && !memcmp(c,&pll_authority,sizeof(*c)));
    configASSERT(xTaskGetCurrentTaskHandle()==bootstrap_task);
    /* The whole-platform provider is modeled. Only registry and mutex
     * operations here are real; no claim of remote/DRAM physical proof. */
    enum a733_clock_result r=a733_clock_acquire(&registry,c->owner,shared,exclusive,c->deadline_us,&out->clock);
    configASSERT(r==A733_CLOCK_OK);out->whole_platform=77;return A733_NPU_PLL_READY;
}
static int pll_enter(void *cookie,uint64_t d)
{configASSERT(cookie==&pll_authority);return a733_platform_lock_enter(&locks,A733_PLATFORM_REGISTER_LOCK,d);}
static int pll_leave(void *cookie)
{
    configASSERT(cookie==&pll_authority && !a733_npu_pll_ready(&pll));
    a733_platform_lock_leave(&locks,A733_PLATFORM_REGISTER_LOCK);return 1;
}
static void pll_retain(void *cookie,const struct a733_npu_pll_resources *a,
    const struct a733_npu_pll_resources *b,unsigned held)
{(void)cookie;(void)a;(void)b;(void)held;configASSERT(0);}
static int pll_now(void *cookie,uint64_t *out)
{configASSERT(cookie==&pll_authority && !a733_npu_pll_ready(&pll));return a733_counter_clock_now(&clock_source,out);}
static int pll_read(void *cookie,uintptr_t address,uint32_t *out)
{
    configASSERT(cookie==&pll_authority && xSemaphoreGetMutexHolder(locks.mutex[0])==bootstrap_task);
    ++pll_reads;
    if(address==0x07090160)*out=0x10000002;
    else *out=*reg(address);
    return 1;
}
static int pll_write(void *cookie,uintptr_t address,uint32_t value)
{
    configASSERT(cookie==&pll_authority && xSemaphoreGetMutexHolder(locks.mutex[0])==bootstrap_task);
    configASSERT(address==CCU+0x2a0 || address==CCU+0x1920);++pll_writes;
    if(address==CCU+0x2a0 && (value&(1u<<29)))value|=1u<<28;
    *reg(address)=value;return 1;
}
static void handoff_client(void *unused)
{
    (void)unused;
    configASSERT(npu_service_activate(&service)==NPU_SERVICE_DENIED);
    configASSERT(a733_npu_startup_poll(&startup)==A733_NPU_PLATFORM_RETAINED && !startup.failed);
    xTaskNotifyGive(bootstrap_task);configASSERT(ulTaskNotifyTake(pdTRUE,1000)==1);
    if(!handoff_mode) {
        struct npu_service_request request={NPU_SERVICE_STATUS,0,0};
        uint64_t ticket;struct npu_service_reply reply;
        configASSERT(npu_service_send(&service,&request,10,&ticket));
        configASSERT(npu_service_collect(&service,ticket,&reply,100) && reply.result==NPU_SERVICE_OK);
    } else configASSERT(!atomic_load(&backend_calls));
    xTaskNotifyGive(bootstrap_task);vTaskSuspend(NULL);configASSERT(0);
}
static void handoff_bootstrap(void *unused)
{
    (void)unused;configASSERT(ulTaskNotifyTake(pdTRUE,1000)==1);
    configASSERT(a733_npu_startup_poll(&startup)==A733_NPU_PLATFORM_PENDING);
    configASSERT(!startup.clock_token.id && !atomic_load(&platform_reads) && !rail_reads);
    configASSERT(npu_service_activate(&service)==NPU_SERVICE_BUSY);
    /* No PMIC owner contends here; the first adopted startup call reaches the
     * first DCDC2 snapshot. */
    atomic_store(&holder_done,1);
    enum a733_npu_pll_result pr=A733_NPU_PLL_PENDING;
    cost_first=xTaskGetTickCount();
    for(unsigned i=0;i<50 && pr!=A733_NPU_PLL_READY;++i) {
        ++cost_pll;pr=a733_npu_pll_poll(&pll);configASSERT(pr!=A733_NPU_PLL_RETAINED);
        if(pr!=A733_NPU_PLL_READY)vTaskDelay(1);
    }
    configASSERT(pr==A733_NPU_PLL_READY && pll_reads && pll_writes);previous=pll.resources.clock;
    if(handoff_mode==1)++pll.config.handoff;
    else {
        atomic_store(&registry.busy,1);
        configASSERT(a733_npu_startup_poll(&startup)==A733_NPU_PLATFORM_BUSY);
        atomic_store(&registry.busy,0);
        configASSERT(!publication_count && !startup.clock_token.id && !pll.transferred);
    }
    ++cost_startup;enum a733_npu_platform_result r=a733_npu_startup_poll(&startup);
    if(handoff_mode==1 || handoff_mode==2) {
        configASSERT(r==A733_NPU_PLATFORM_RETAINED && startup.failed && !rail_reads);
        if(handoff_mode==1)configASSERT(!publication_count && !startup.clock_token.id &&
            a733_clock_token_status(&registry,&previous)==A733_CLOCK_OK);
        else configASSERT(publication_count==1 && startup.pll_transferred && pll.transferred &&
            startup.clock_token.id!=previous.id && a733_clock_token_status(&registry,&startup.clock_token)==A733_CLOCK_OK);
    } else {
        configASSERT(r==A733_NPU_PLATFORM_PENDING && publication_count==1 && startup.pll_transferred && pll.transferred);
        configASSERT(startup.clock_token.id!=previous.id && startup.clock_token.exclusive==A733_CLOCK_NPU);
        configASSERT(startup.clock_token.shared==(previous.shared|A733_CLOCK_PLL_NPU));
        configASSERT(a733_clock_token_status(&registry,&previous)==A733_CLOCK_INVALID);
        configASSERT(a733_clock_token_status(&registry,&startup.clock_token)==A733_CLOCK_OK);
        struct a733_clock_token denied;
        configASSERT(a733_clock_acquire(&registry,44,0,A733_CLOCK_PLL_NPU,deadline(),&denied)==A733_CLOCK_BUSY);
        configASSERT(a733_clock_acquire(&registry,44,0,A733_CLOCK_NPU,deadline(),&denied)==A733_CLOCK_BUSY);
        configASSERT(rail_reads==6 && startup.settle_until && !atomic_load(&platform_reads));
        if(handoff_mode==3) {
            pll.cookie=0; /* Bound provider identity must remain immutable. */
            configASSERT(a733_npu_startup_poll(&startup)==A733_NPU_PLATFORM_RETAINED && startup.failed);
        } else {
            for(unsigned i=0;i<80 && r!=A733_NPU_PLATFORM_READY;++i) {
                ++cost_startup;r=a733_npu_startup_poll(&startup);configASSERT(r!=A733_NPU_PLATFORM_RETAINED);
                if(r!=A733_NPU_PLATFORM_READY)vTaskDelay(1);
            }
            configASSERT(r==A733_NPU_PLATFORM_READY && rail_reads==12 && closes==2);
            configASSERT(npu_service_activate(&service)==NPU_SERVICE_OK);
            /* One PLL or startup call per tick from PLL acquisition to READY. */
            platform_puts("COST pll_polls=");platform_hex(cost_pll);platform_puts(" startup_polls=");platform_hex(cost_startup);
            platform_puts(" ticks=");platform_hex(xTaskGetTickCount()-cost_first);platform_puts("\n");
        }
    }
    if(handoff_mode) {
        unsigned reads=atomic_load(&platform_reads),writes=atomic_load(&platform_writes),rails=rail_reads;
        configASSERT(npu_service_activate(&service)==NPU_SERVICE_FAULT);
        configASSERT(a733_npu_startup_poll(&startup)==A733_NPU_PLATFORM_RETAINED);
        configASSERT(reads==atomic_load(&platform_reads) && writes==atomic_load(&platform_writes) && rails==rail_reads);
        configASSERT(!atomic_load(&backend_calls) && pll.resources.whole_platform==77);
    }
    xTaskNotifyGive(client_task);configASSERT(ulTaskNotifyTake(pdTRUE,1000)==1);
    platform_puts(!handoff_mode ? "PASS NPU PLL atomic adoption through real startup and cross-core activation\n" :
        handoff_mode==1 ? "PASS pre-publication source mutation retains exclusive authority without startup I/O\n" :
        handoff_mode==2 ? "PASS late publication retains reachable fresh token and silent service\n" :
        "PASS post-publication provider mutation retains adopted authority without further I/O\n");
    platform_exit(0);
}
BaseType_t npu_clock_handoff_task_privileged(TaskFunction_t entry,void *argument)
{return entry==handoff_bootstrap || npu_service_task_privileged(entry,argument);}
static unsigned handoff_selection(void)
{
    char command[128]={0};uintptr_t parameters[2]={(uintptr_t)command,sizeof(command)};
    register uintptr_t operation __asm__("x0")=0x15;
    register uintptr_t arguments __asm__("x1")=(uintptr_t)parameters;
    __asm__ volatile("hlt #0xf000" : "+r"(operation) : "r"(arguments) : "memory");
    configASSERT(operation==0);
    for(unsigned i=0;i+6<sizeof(command);++i) {
        if(!memcmp(command+i,"before",6))return 1;
        if(!memcmp(command+i,"late",4))return 2;
        if(!memcmp(command+i,"after",5))return 3;
    }
    return 0;
}
void kernel_main(void)
{
    platform_init();
    handoff_mode=handoff_selection();
    uint64_t frequency;__asm__ volatile("mrs %0,cntfrq_el0":"=r"(frequency));
    configASSERT(a733_counter_clock_init(&clock_source,(uint32_t)frequency));
    configASSERT(a733_clock_leases_init(&registry,a733_counter_clock_now,&clock_source));
    configASSERT(a733_platform_locks_init(&locks,a733_counter_clock_now,&clock_source));
    configASSERT(a733_npu_startup_resources_init(&resources,&clock_source,&locks));
    struct a733_npu_handoff h={
        .allowed={.fabric={0x03000001,0x82000001,0x84000000},.rate_hz={300000000,400000000,400000000},
            .owner=7,.handoff=123,.deadline_us=deadline()},
        .backend={device_read,device_write,microseconds,delay,barrier,publish,finish},.backend_cookie=&command_token,
        .image={0x44000000,0x5000,0x44000000,1},.commands={{0x100000,A733_NPU_FETCH_BYTES,&command_token}},
        .controller=0x03600000,.evidence=123,.command_admission=234,.established=A733_NPU_HANDOFF_REQUIRED,.command_count=1};
    configASSERT(a733_npu_startup_init(&startup,&h,&resources,&registry));
    ccu[0]=0xf8000000|(99<<16)|(99<<8);ccu[0xa0/4]=0xfe000000|(1<<20)|(2<<16)|(99<<8)|(4<<2);
    ccu[0x2a0/4]=0xf8000000|(1<<20)|(81<<8);ccu[0x2ac/4]=0x30000000;ccu[0x2b0/4]=0xb0000;
    ccu[0x1908/4]=0x0fff0fff;ccu[0x1988/4]=0x0fff0000;ccu[0x1920/4]=0x10001;ccu[0x19a0/4]=0x10000;
    ccu[0x500/4]=h.allowed.fabric[0];ccu[0x580/4]=h.allowed.fabric[1];ccu[0x588/4]=h.allowed.fabric[2];
    ccu[0x5c0/4]=0x010000ff;ccu[0x5e0/4]=0x41055800;ccu[0xb04/4]=0x80000;
    ccu[0xc00/4]=0x80000000;ccu[0xc0c/4]=0x10001;
    pll_authority=(struct a733_npu_pll_config){.owner=7,.handoff=123,.deadline_us=h.allowed.deadline_us,
        .mode=A733_NPU_PLL_ENABLED_RETUNE,.initial_control=ccu[0x2a0/4]};
    const unsigned offsets[]={0x500,0x580,0x588,0xa80,0xa88,0xa8c,0xb00,0xb04,0xc00,0xc0c,0x1b10,0x1b14,0x1b1c};
    for(unsigned i=0;i<A733_NPU_PLL_SNAPSHOTS;++i)pll_authority.consumers[i]=ccu[offsets[i]/4];
    struct a733_npu_pll_operations po={pll_acquire,pll_enter,pll_leave,pll_retain,pll_now,pll_read,pll_write};
    configASSERT(a733_npu_pll_init(&pll,&pll_authority,&po,&pll_authority));
    configASSERT(a733_npu_pll_bind_registry(&pll,&registry));
    copied_pll=pll;configASSERT(!a733_npu_startup_bind_pll(&startup,&copied_pll));
    configASSERT(a733_npu_startup_bind_pll(&startup,&pll));
    configASSERT(!a733_npu_startup_bind_pll(&startup,&pll));
    bootstrap_task=xTaskCreateStaticAffinitySet(handoff_bootstrap,"npu-adopt",3072,NULL,3,stacks[0],&task_storage[0],1);
    client_task=xTaskCreateStaticAffinitySet(handoff_client,"npu-client",3072,NULL,2,stacks[2],&task_storage[2],2);
    configASSERT(bootstrap_task && client_task && a733_npu_startup_bind_task(&startup,bootstrap_task));
    struct npu_service_activation activation={bootstrap_task,prepared,&startup};
    configASSERT(npu_service_init_deferred(&service,&h.backend,h.backend_cookie,&h.image,h.commands,1,5,3,&activation));
    configASSERT(npu_service_bind(&service,0,client_task,0));
    vTaskStartScheduler();platform_panic("scheduler returned",0);
}
