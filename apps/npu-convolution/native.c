#include "a733_npu_application.h"
#ifdef SUNSTONE_NPU_INTERRUPT
#include "../npu-interrupt/irq.h"
#endif
#include <string.h>
#include "aarch64_device_memory.h"
#ifdef SUNSTONE_NPU_SINGLE_CORE
#include "a733_usb_platform.h"
#include "a733_usb_platform_native.h"
#endif
#include "platform.h"
#include "result.h"
#include "cached.h"
#ifdef SUNSTONE_NPU_SUPERVISED
#include "clock.h"
#endif

static struct {
#ifdef SUNSTONE_NPU_SUPERVISED
    struct npu_application_clock pll;
#endif
    struct a733_counter_clock clock;
    struct a733_clock_leases registry;
    struct a733_platform_locks locks;
    struct a733_npu_startup_resources resources;
    struct a733_npu_startup startup;
    struct a733_npu_handoff handoff;
    struct a733_pins_native pin_native;
    struct a733_pins pins;
    struct a733_rail_leases rails;
    struct a733_pmic_native pmic_native;
    struct a733_pmic pmic;
    struct a733_pmic_lifetime pmic_lifetime;
    struct a733_pmic_config pmic_config;
    struct a733_npu_reserved reserved;
    struct device_memory_lease lease;
    struct device_memory_extent extent;
    struct device_memory_allocation allocation;
    struct device_memory_pool *pool;
    struct device_memory_extent backing;
#ifdef SUNSTONE_NPU_SINGLE_CORE
    struct a733_usb_platform usb_platform;
    TaskHandle_t usb_task;
#else
    struct a733_clock_token usb;
#endif
    StaticTask_t task[2];
    StackType_t stack[2][4096] __attribute__((aligned(16)));
    TaskHandle_t pmic_task,bootstrap_task,client;
    atomic_uint ready,worker_admitted,stopped;
#ifdef SUNSTONE_NPU_FINITE_RETURN
    atomic_uint pmic_stop,pmic_parked,quiescent;
#endif
    unsigned used,creating;
} app;
atomic_uint a733_npu_application_failed;
static int preparation_admitted(void)
{
#ifdef SUNSTONE_NPU_SINGLE_CORE
    /* The SRE static-task constructor restores IRQ delivery before scheduling.
     * No EL1 timer is armed yet; restore the preparation window's mask before
     * calling the shared startup authority. */
    if(xTaskGetSchedulerState()!=taskSCHEDULER_NOT_STARTED)return 0;
    __asm__ volatile("msr daifset,#15":::"memory");
#endif
    return a733_smp_device_preparation_admitted();
}
static void pmic_task(void *);
static void bootstrap(void *);
const struct a733_npu_application_boot *a733_npu_application_boot_record(void)
{
#ifdef SUNSTONE_NPU_SUPERVISED
    const struct npu_session *s=npu_session_get();return s ? &s->boot : NULL;
#else
    return &a733_npu_application_boot;
#endif
}

void a733_npu_application_fail(unsigned reason)
{
    unsigned expected=0;
    atomic_compare_exchange_strong_explicit(&a733_npu_application_failed,&expected,
        reason ? reason : 1,memory_order_release,memory_order_relaxed);
}
static _Noreturn void fatal(void *unused)
{
    (void)unused;a733_npu_application_fail(1);
    if(xTaskGetSchedulerState()==taskSCHEDULER_RUNNING)vTaskSuspend(NULL);
    platform_panic("NPU application retained",0);
}
unsigned a733_npu_application_health(void)
{
    if(!app.used)return 0;
    if(atomic_load_explicit(&app.reserved.convolution.failed,memory_order_acquire) ||
       atomic_load_explicit(&app.clock.failed,memory_order_acquire) ||
       atomic_load_explicit(&app.registry.failed,memory_order_acquire) ||
       atomic_load_explicit(&app.locks.retained,memory_order_acquire))
        a733_npu_application_fail(17);
    return !atomic_load_explicit(&a733_npu_application_failed,memory_order_acquire);
}
static int live(void)
{return a733_npu_application_health();}
static int image(const void *p,size_t bytes,int write)
{
    extern unsigned char __device_pool_start[];
    uintptr_t a=(uintptr_t)p,limit=(uintptr_t)__device_pool_start;
    if(!p || !bytes || a<0x41000000 || a>=limit || bytes>limit-a)return 0;
    for(uintptr_t page=a&~(uintptr_t)4095;page<a+bytes;page+=4096) {
        struct aarch64_translation t;
        if(!aarch64_device_translate(page,write,&t) || t.physical!=page ||
           t.attributes!=0xff || t.shareability!=3)return 0;
    }
    return 1;
}
static int parent(void)
{
    const struct a733_npu_reserved *s=&app.reserved;
    if(!s->admitted || !s->pool || s->pool!=app.pool || !s->pool->initialized ||
       s->pool->backing.cpu!=app.backing.cpu || s->pool->backing.physical!=app.backing.physical ||
       s->pool->backing.bytes!=app.backing.bytes || s->lease.pool!=app.lease.pool ||
       s->lease.slot!=app.lease.slot || s->lease.owner!=app.lease.owner ||
       s->lease.generation!=app.lease.generation || app.lease.slot>=DEVICE_MEMORY_MAX_ALLOCATIONS ||
       s->extent.cpu!=app.extent.cpu || s->extent.physical!=app.extent.physical ||
       s->extent.bytes!=A733_NPU_RESERVED_BYTES || s->extent.bytes!=app.extent.bytes)return 0;
    const struct device_memory_allocation *a=&s->pool->allocations[app.lease.slot];
    return a->owner==app.allocation.owner && a->generation==app.allocation.generation &&
        a->first==app.allocation.first && a->pages==app.allocation.pages && a->state==DEVICE_MEMORY_DEVICE;
}
static int worker(void *cookie)
{
    uint64_t el,now;__asm__ volatile("mrs %0,CurrentEL":"=r"(el));
    if(el!=4 || cookie!=&app.reserved.convolution.native || !live() || !parent() ||
       !atomic_load_explicit(&app.worker_admitted,memory_order_acquire) ||
       xTaskGetSchedulerState()!=taskSCHEDULER_RUNNING ||
       xTaskGetCurrentTaskHandle()!=app.reserved.convolution.service.worker ||
       !a733_counter_clock_now(&app.clock,&now) || now>=app.handoff.allowed.deadline_us) {
        a733_npu_application_fail(2);return 0;
    }
    return 1;
}
static int read_reg(void *p,unsigned off,uint32_t *v)
{return worker(p) && a733_npu_native_operations.read(p,off,v);}
static int write_reg(void *p,unsigned off,uint32_t v)
{return worker(p) && a733_npu_native_operations.write(p,off,v);}
static uint64_t ticks(void *p)
{if(!worker(p))fatal(NULL);return a733_npu_native_operations.microseconds(p);}
static void delay(void *p,unsigned us)
{if(!worker(p))fatal(NULL);a733_npu_native_operations.delay(p,us);}
static void barrier(void *p)
{if(!worker(p))fatal(NULL);a733_npu_native_operations.barrier(p);}
static int publish(void *p,const struct a733_npu_mmu_image *m,const struct a733_npu_command *c,int boot)
{return worker(p) && a733_npu_native_operations.publish(p,m,c,boot);}
static int finish(void *p,const struct a733_npu_command *c,int canceled)
{return worker(p) && a733_npu_native_operations.finish(p,c,canceled);}
const struct a733_npu_ops a733_npu_application_operations={read_reg,write_reg,ticks,delay,barrier,publish,finish};

static int pin_enter(void *cookie,uint64_t deadline)
{
    return cookie==&app && live() && xTaskGetCurrentTaskHandle()==app.pmic_task &&
        a733_platform_lock_enter(&app.locks,A733_PLATFORM_REGISTER_LOCK,deadline);
}
static void pin_leave(void *cookie)
{if(cookie!=&app)fatal(NULL);a733_platform_lock_leave(&app.locks,A733_PLATFORM_REGISTER_LOCK);}
static int pin_now(void *cookie,uint64_t *out)
{return cookie==&app && live() && a733_counter_clock_now(&app.clock,out);}

BaseType_t a733_npu_application_task_privileged(TaskFunction_t entry,void *argument)
{
    return (app.creating && argument==&app && (entry==pmic_task || entry==bootstrap)) ||
        npu_service_task_privileged(entry,argument) ? pdTRUE : pdFALSE;
}
static void pmic_task(void *argument)
{
    if(argument!=&app)fatal(NULL);
    for(;;) {
#ifdef SUNSTONE_NPU_FINITE_RETURN
        if(atomic_load_explicit(&app.pmic_stop,memory_order_acquire)) {
            unsigned idle=!app.pmic.held && !app.pmic.retained && !app.pmic.failed &&
                !atomic_load_explicit(&app.pmic.busy,memory_order_acquire) &&
                app.pmic.active==A733_PMIC_REQUESTS;
            for(unsigned i=0;i<A733_PMIC_REQUESTS;++i)
                if(app.pmic.slots[i].state || app.pmic.slots[i].borrow.id)idle=0;
            if(idle) {
                atomic_store_explicit(&app.pmic_parked,1,memory_order_release);
                for(;;)vTaskSuspend(NULL);
            }
        }
#endif
        if(!live())break;
        enum a733_pmic_status r=a733_pmic_poll(&app.pmic);
        if(r!=A733_PMIC_READY && r!=A733_PMIC_PENDING && r!=A733_PMIC_BUSY)break;
        vTaskDelay(1);
    }
    a733_npu_application_fail(3);vTaskSuspend(NULL);fatal(NULL);
}
static void bootstrap(void *argument)
{
    if(argument!=&app)fatal(NULL);
    for(;;) {
        uint64_t now;
        if(!live() || !parent() || !a733_counter_clock_now(&app.clock,&now) ||
           now>=app.handoff.allowed.deadline_us)break;
        #ifdef SUNSTONE_NPU_SUPERVISED
        if(!a733_npu_pll_ready(&app.pll.pll)) {
            /* Observe the real retained DCDC2 supply before any PLL change. */
            int rail=a733_npu_startup_supply_sample(&app.startup);
            if(rail<0)break;
            if(!rail){vTaskDelay(1);continue;}
            enum a733_npu_pll_result pll=a733_npu_pll_poll(&app.pll.pll);
            if(pll==A733_NPU_PLL_RETAINED)break;
            if(pll!=A733_NPU_PLL_READY){vTaskDelay(1);continue;}
        }
#endif
        enum a733_npu_platform_result r=npu_convolution_startup_poll(&app.reserved.convolution);
        if(r==A733_NPU_PLATFORM_RETAINED)break;
        if(r==A733_NPU_PLATFORM_READY) {
            /* Worker may wake on another core inside activate(). Its admission
             * therefore precedes activation; client readiness follows success. */
            atomic_store_explicit(&app.worker_admitted,1,memory_order_release);
            enum npu_service_result a=npu_service_activate(&app.reserved.convolution.service);
            if(a==NPU_SERVICE_OK) {
                atomic_store_explicit(&app.ready,1,memory_order_release);
#if defined(SUNSTONE_NPU_FINITE_RETURN) || defined(SUNSTONE_NPU_RUNTIME_INPUT)
                for(;;) {
                    if(!live() || !parent() || !a733_counter_clock_now(&app.clock,&now) ||
                       now>=app.handoff.allowed.deadline_us)break;
                    if(atomic_load_explicit(&app.reserved.convolution.closed,memory_order_acquire)) {
                        enum a733_npu_platform_result stopped=npu_convolution_stop_poll(&app.reserved.convolution);
                        if(stopped==A733_NPU_PLATFORM_RETAINED)break;
                        if(stopped==A733_NPU_PLATFORM_READY) {
                            atomic_store_explicit(&app.stopped,1,memory_order_release);
                            vTaskSuspend(NULL);fatal(NULL);
                        }
                    }
                    vTaskDelay(1);
                }
                break;
#else
                vTaskSuspend(NULL);fatal(NULL);
#endif
            }
            if(a!=NPU_SERVICE_BUSY)break;
        }
        vTaskDelay(1);
    }
    a733_npu_application_fail(4);vTaskSuspend(NULL);fatal(NULL);
}
unsigned a733_npu_application_ready(void)
{return atomic_load_explicit(&app.ready,memory_order_acquire) && live();}
unsigned a733_npu_application_stopped(void)
{return atomic_load_explicit(&app.stopped,memory_order_acquire) && live();}
#ifdef SUNSTONE_NPU_FINITE_RETURN
#if !defined(SUNSTONE_NPU_SINGLE_CORE) || !defined(SUNSTONE_NPU_SUPERVISED)
#error "Finite NPU quiescence requires the supervised single-core image"
#endif
static int finite_irq_quiet(void)
{
#ifdef SUNSTONE_NPU_INTERRUPT
    if(!npu_service_irq_stopped(&app.reserved.convolution.service))return 0;
#endif
    /* NPU INTID97: distributor bank3, bit1. Only observe the established
     * masked interrupt; never acknowledge or change inherited GIC state. */
    const uintptr_t addresses[]={0x03400000,0x0340010c,0x0340020c,0x0340030c};
    uint32_t value[4];
    for(unsigned i=0;i<4;++i) {
        struct aarch64_translation t;
        if(!aarch64_device_translate(addresses[i],0,&t) ||
           t.physical!=addresses[i] || t.attributes!=0)return -1;
        __asm__ volatile("dsb sy":::"memory");
        value[i]=*(volatile uint32_t *)addresses[i];
        __asm__ volatile("dsb sy":::"memory");
    }
    return !(value[0]&UINT32_C(0x80000000)) && !((value[1]|value[2]|value[3])&2);
}
unsigned a733_npu_application_quiesce(void)
{
    if(xTaskGetSchedulerState()!=taskSCHEDULER_RUNNING ||
       xTaskGetCurrentTaskHandle()!=app.client)return 0;
    if(!a733_npu_application_stopped() || !parent() ||
       !atomic_load_explicit(&app.reserved.convolution.closed,memory_order_acquire) ||
       !app.reserved.convolution.service.worker || !app.bootstrap_task || !app.pmic_task ||
       app.reserved.convolution.service.worker==app.client || app.bootstrap_task==app.client ||
       app.pmic_task==app.client || app.reserved.convolution.service.worker==app.pmic_task ||
       app.reserved.convolution.service.worker==app.bootstrap_task || app.bootstrap_task==app.pmic_task)goto failed;
    /* CLOSED has no further register callbacks. On this single CPU, suspend
     * the original worker and bootstrap before checking provider storage. */
    vTaskSuspend(app.reserved.convolution.service.worker);
    vTaskSuspend(app.bootstrap_task);
    atomic_store_explicit(&app.pmic_stop,1,memory_order_release);
    while(!atomic_load_explicit(&app.pmic_parked,memory_order_acquire)) {
        uint64_t now;
        if(!live() || !a733_counter_clock_now(&app.clock,&now) ||
           now>=app.handoff.allowed.deadline_us)goto failed;
        vTaskDelay(1);
    }
    vTaskSuspend(app.pmic_task);
    if(app.pmic.held || app.pmic.retained || app.pmic.failed ||
       atomic_load_explicit(&app.pmic.busy,memory_order_acquire) ||
       app.pmic.active!=A733_PMIC_REQUESTS ||
       atomic_load_explicit(&app.locks.retained,memory_order_acquire))goto failed;
    for(unsigned i=0;i<A733_PMIC_REQUESTS;++i)
        if(app.pmic.slots[i].state || app.pmic.slots[i].borrow.id)goto failed;
    if(xSemaphoreGetMutexHolder(app.locks.mutex[0]) ||
       xSemaphoreGetMutexHolder(app.locks.mutex[1]))goto failed;
    int quiet;
    do {
        uint64_t now;
        quiet=finite_irq_quiet();
        if(quiet<0 || !live() || !a733_counter_clock_now(&app.clock,&now) ||
           now>=app.handoff.allowed.deadline_us)goto failed;
        if(!quiet)vTaskDelay(1);
    }while(!quiet);
    atomic_store_explicit(&app.quiescent,1,memory_order_release);
    return 1;
failed:
    a733_npu_application_fail(20);return 0;
}
unsigned a733_npu_application_quiescent(void)
{return atomic_load_explicit(&app.quiescent,memory_order_acquire) &&
    atomic_load_explicit(&app.pmic_parked,memory_order_acquire) &&
    a733_npu_application_stopped() && finite_irq_quiet()==1;}
#endif
struct npu_convolution *a733_npu_application_client(void)
{
    return app.used && xTaskGetSchedulerState()==taskSCHEDULER_RUNNING &&
        xTaskGetCurrentTaskHandle()==app.client && a733_npu_application_ready() ? &app.reserved.convolution : NULL;
}
int a733_npu_application_init(TaskHandle_t client)
{
    const struct a733_npu_application_boot *b=a733_npu_application_boot_record();
    if(!b)return 0;
    const struct a733_npu_application_profile *p=&b->profile;
    if(app.used || !client || xTaskGetSchedulerState()!=taskSCHEDULER_NOT_STARTED ||
       !preparation_admitted() || !a733_npu_application_observer_admit() ||
       !image(&app,sizeof(app),1) || !image(b,sizeof(*b),0) ||
       (!a733_npu_application_model_bytes || a733_npu_application_model_bytes>NPU_CONVOLUTION_MODEL_CAPACITY) ||
       (!a733_npu_application_input_bytes || a733_npu_application_input_bytes>NPU_CONVOLUTION_INPUT_CAPACITY) ||
       (!a733_npu_application_reference_bytes || a733_npu_application_reference_bytes>NPU_CONVOLUTION_OUTPUT_CAPACITY) ||
       !image(a733_npu_application_model,a733_npu_application_model_bytes,0) ||
       !image(a733_npu_application_input,a733_npu_application_input_bytes,0) ||
       !image(a733_npu_application_reference,a733_npu_application_reference_bytes,0) ||
       !p->npu_owner || !p->pmic_owner || !p->usb_owner || !p->bridge_owner ||
       p->npu_owner==p->pmic_owner || p->npu_owner==p->usb_owner || p->pmic_owner==p->usb_owner ||
       p->bridge_owner==p->npu_owner || p->bridge_owner==p->pmic_owner || p->bridge_owner==p->usb_owner ||
       b->npu.owner!=p->npu_owner || b->npu.handoff!=p->npu_evidence || b->npu.deadline_us ||
       b->pmic.owner!=p->pmic_owner || b->pmic.identity!=p->pmic_evidence || b->pmic.startup_deadline)return 0;
    app.used=1;app.client=client;
    uint64_t deadline=a733_observe_identity.deadline/24;
    if(!a733_counter_clock_init(&app.clock,24000000) ||
       !a733_clock_leases_init(&app.registry,a733_counter_clock_now,&app.clock) ||
       !a733_platform_locks_init(&app.locks,a733_counter_clock_now,&app.clock) ||
       !a733_npu_startup_resources_init(&app.resources,&app.clock,&app.locks))goto failed;
    const struct a733_pins_locks pin_locks={pin_enter,pin_leave,pin_now,&app};
    const struct a733_rail_binding rail_binding={&app.registry,&app.resources.manager,
        p->bridge_owner,a733_counter_clock_now,&app.clock};
    if(!a733_pins_native_init(&app.pin_native,&pin_locks) ||
       !a733_pins_init_banks(&app.pins,&app.pin_native.operations,&app.pin_native,&b->pins) ||
       !a733_rail_leases_init(&app.rails,&rail_binding) ||
#ifndef SUNSTONE_NPU_SINGLE_CORE
       a733_clock_acquire(&app.registry,p->usb_owner,A733_CLOCK_FABRIC,
           A733_CLOCK_USB0|A733_CLOCK_USB01_REF,deadline,&app.usb)!=A733_CLOCK_OK
#else
       0
#endif
       )goto failed;
    if(!a733_npu_reserved_prepare(&app.reserved,p->npu_owner,a733_npu_application_model,
        a733_npu_application_model_bytes,0x03600000,fatal,NULL))goto failed;
    app.lease=app.reserved.lease;app.extent=app.reserved.extent;
    app.pool=app.reserved.pool;app.backing=app.pool->backing;
    app.allocation=*device_memory_lookup(app.reserved.pool,app.lease);
    app.creating=1;
    app.pmic_task=xTaskCreateStaticAffinitySet(pmic_task,"npu-pmic",4096,&app,4,app.stack[0],&app.task[0],1);
    app.bootstrap_task=xTaskCreateStaticAffinitySet(bootstrap,"npu-boot",4096,&app,4,app.stack[1],&app.task[1],1);
    app.creating=0;
    if(!app.pmic_task || !app.bootstrap_task)goto failed;
    app.pmic_config=b->pmic;app.pmic_config.startup_deadline=deadline;
    if(!a733_pmic_native_init(&app.pmic_native,&app.locks,app.pmic_task) ||
       !a733_pmic_init(&app.pmic,&app.pmic_config,&app.pmic_native.operations,&app.pmic_native,
           &app.registry,&app.rails,&app.pins) ||
       a733_pmic_identity(&app.pmic,&app.pmic_lifetime)!=A733_PMIC_READY)goto failed;
    struct a733_npu_handoff base={.allowed=b->npu,.controller=0x03600000,
        .evidence=p->npu_evidence,.command_admission=p->command_admission,
        .established=(uint32_t)p->npu_established,.pck_client=0};
    base.allowed.deadline_us=deadline;
    if(!npu_convolution_handoff(&app.reserved.convolution,&base,&app.handoff) ||
       !a733_npu_startup_init(&app.startup,&app.handoff,&app.resources,&app.registry) ||
       !a733_npu_startup_bind_pmic(&app.startup,&app.pmic,&app.pmic_lifetime) ||
       !a733_npu_startup_bind_task(&app.startup,app.bootstrap_task) ||
#ifdef SUNSTONE_NPU_SUPERVISED
       !npu_application_clock_init(&app.pll,npu_session_get(),&app.registry,&app.locks,&app.clock,
           app.bootstrap_task,deadline) || !a733_npu_startup_bind_pll(&app.startup,&app.pll.pll) ||
#endif
       !npu_convolution_bind(&app.reserved.convolution,&app.startup,client,4,
#ifdef SUNSTONE_NPU_SINGLE_CORE
           1
#else
           2
#endif
       ) ||
#ifdef SUNSTONE_NPU_INTERRUPT
       !npu_application_irq_bind(&app.reserved.convolution.service) ||
#endif
       !preparation_admitted())goto failed;
    return 1;
failed:
    a733_npu_application_fail(5);return 0;
}

#ifdef SUNSTONE_NPU_SINGLE_CORE
static int usb_transport_live(void)
{
    /* A failed convolution still needs its terminal diagnostic frame. Keep
     * USB conditional on its own authenticated lifetime and shared resources;
     * a retained clock, register or I2C lock still forbids further USB work. */
    return app.used && app.registry.initialized && app.locks.initialized &&
        !atomic_load_explicit(&app.clock.failed,memory_order_acquire) &&
        !atomic_load_explicit(&app.registry.failed,memory_order_acquire) &&
        !atomic_load_explicit(&app.locks.retained,memory_order_acquire) &&
        a733_npu_application_observer_admit();
}
static int usb_now(void *cookie,uint64_t *out)
{return cookie==&app&&usb_transport_live()&&a733_counter_clock_now(&app.clock,out);}
static int usb_authorize(void *cookie,const struct a733_usb_platform_config *c,const struct a733_clock_token *t)
{
    const struct a733_npu_application_boot *b=a733_npu_application_boot_record();
    if(cookie!=&app||!usb_transport_live()||!b||xTaskGetCurrentTaskHandle()!=app.usb_task||
       !a733_npu_application_observer_admit()||c->owner!=b->profile.usb_owner||
       c->handoff!=b->profile.usb_evidence||c->cold_reference!=A733_USB_ROM_DEVICE||
       c->deadline_us!=a733_observe_identity.deadline/24||
       memcmp(c->fabric,b->npu.fabric,sizeof(c->fabric))||
       memcmp(c->rate_hz,b->npu.rate_hz,sizeof(c->rate_hz))||
       a733_clock_token_status(&app.registry,t)!=A733_CLOCK_OK)return 0;
    const uintptr_t mmio[]={0x02002000,0x04100000,0x07090160};
    for(unsigned i=0;i<3;++i){struct aarch64_translation t;
        if(!aarch64_device_translate(mmio[i],1,&t)||t.physical!=mmio[i]||t.attributes||t.shareability!=2)return 0;}
    /* Authenticated external admission owns persistent USB rails, IRQ exclusion,
     * the ROM PIO predecessor and absence of outstanding external DMA. */
    return 1;
}
static int usb_enter(void *cookie,uint64_t deadline)
{return cookie==&app&&usb_transport_live()&&xTaskGetCurrentTaskHandle()==app.usb_task&&
    a733_platform_lock_enter(&app.locks,A733_PLATFORM_REGISTER_LOCK,deadline);}
static void usb_leave(void *cookie)
{if(cookie!=&app)fatal(NULL);a733_platform_lock_leave(&app.locks,A733_PLATFORM_REGISTER_LOCK);}
static void usb_retain(void *cookie)
{if(cookie==&app)a733_platform_lock_retain(&app.locks,A733_PLATFORM_REGISTER_LOCK);a733_npu_application_fail(18);}
int npu_single_core_usb_poll(void)
{
    if(!usb_transport_live())return -1;
    if(!app.usb_task){
        app.usb_task=xTaskGetCurrentTaskHandle();
        const struct a733_npu_application_boot *b=a733_npu_application_boot_record();
        struct a733_usb_platform_config c={.owner=b->profile.usb_owner,.handoff=b->profile.usb_evidence,
            .deadline_us=a733_observe_identity.deadline/24,.cold_reference=A733_USB_ROM_DEVICE};
        memcpy(c.fabric,b->npu.fabric,sizeof(c.fabric));memcpy(c.rate_hz,b->npu.rate_hz,sizeof(c.rate_hz));
        const struct a733_usb_platform_ops ops={usb_now,usb_authorize,usb_enter,usb_leave,usb_retain,
            a733_usb_platform_native_read,a733_usb_platform_native_write};
        if(!a733_usb_platform_init(&app.usb_platform,&c,&ops,&app,&app.registry))return -1;
    }
    if(xTaskGetCurrentTaskHandle()!=app.usb_task)return -1;
    enum a733_usb_platform_result r=a733_usb_platform_poll(&app.usb_platform);
    if(r==A733_USB_PLATFORM_PREPARED)return 1;
    return r==A733_USB_PLATFORM_PENDING||r==A733_USB_PLATFORM_BUSY?0:-1;
}
#endif
