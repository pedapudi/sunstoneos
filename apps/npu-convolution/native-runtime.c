/* The supervised single-core finite returning image with the NPU runtime
 * (native-runtime.h). The providers, the PMIC task, the bootstrap's startup
 * and stop sequence and the quiescence proof are those of native.c; the NPU
 * engine is the runtime of drivers/npu/a733_npu_runtime.c with one slot in
 * place of the fixed-model convolution object, so the client submits any
 * admitted network through npu_job_run. */
#include "native-runtime.h"
#include "npu_job.h"
#include "sunstone_crumb.h"
#include "failure.h"
#include "a733_npu_irq.h"
#include <string.h>
#include "a733_mapping_check.h"
#include "a733_power_domains_native.h"
#include "a733_mmio.h"
#include "aarch64_device_memory.h"
#include "platform.h"
#include "result.h"
#include "cached.h"
#include "session.h"

#if !defined(SUNSTONE_NPU_SINGLE_CORE) || !defined(SUNSTONE_NPU_SUPERVISED) || \
    !defined(SUNSTONE_NPU_INTERRUPT) || !defined(SUNSTONE_NPU_FINITE_RETURN) || \
    defined(SUNSTONE_ACCELERATORS) || defined(SUNSTONE_NPU_FINITE_THERMAL) || defined(SUNSTONE_NPU_RESTART)
#error "The NPU runtime finite image is the supervised single-core interrupt image only"
#endif
/* The single-core IRQ binding, compiled for the runtime (apps/npu-finite/runtime-irq.c). */
int npu_finite_runtime_irq_bind(struct npu_runtime *);
/* The parent allocation: whole 2 MiB blocks of image RAM after the stacks,
 * reserved by apps/npu-finite/link.ld when the build defines its size. */
extern unsigned char __npu_runtime_parent_start[], __npu_runtime_parent_end[];
/* Finite clients may prepare several resident commands before scheduling.
 * The default retains the ordinary single-slot runtime behavior. */
__attribute__((weak)) unsigned a733_npu_runtime_resident_slots(void) { return 0; }
__attribute__((weak)) int a733_npu_runtime_resident_prepare(struct npu_runtime *rt)
{ (void)rt; return 0; }

static struct {
    struct a733_counter_clock clock;
    struct a733_clock_leases registry;
    struct a733_platform_locks locks;
    /* The image's only GPU/PCK resource manager. */
    struct a733_gpu_resource_manager gpu;
    struct a733_gpu_resource_native gpu_native;
    struct a733_npu_startup startup;
    struct a733_pins_native pin_native;
    struct a733_pins pins;
    struct a733_rail_leases rails;
    struct a733_pmic_native pmic_native;
    struct a733_pmic pmic;
    struct a733_pmic_lifetime pmic_lifetime;
    struct a733_pmic_config pmic_config;
    /* The power-domain owner; its service task is the PMIC task. */
    struct a733_power_domains domains;
    struct a733_power_domains_native domains_native;
    struct npu_runtime runtime;
    StaticTask_t task[2];
    StackType_t stack[2][4096] __attribute__((aligned(16)));
    TaskHandle_t pmic_task,bootstrap_task,client;
    atomic_uint ready,stopped,pmic_stop,pmic_parked,quiescent;
    unsigned used;
} app;
atomic_uint a733_npu_application_failed;
static int preparation_admitted(void)
{
    /* The SRE static-task constructor restores IRQ delivery before scheduling.
     * No EL1 timer is armed yet; restore the preparation window's mask before
     * calling the shared startup authority. */
    if(xTaskGetSchedulerState()!=taskSCHEDULER_NOT_STARTED)return 0;
    __asm__ volatile("msr daifset,#15":::"memory");
    return a733_smp_device_preparation_admitted();
}
const struct a733_npu_application_boot *a733_npu_application_boot_record(void)
{const struct npu_session *s=npu_session_get();return s ? &s->boot : NULL;}

void a733_npu_application_fail(unsigned reason)
{
    unsigned expected=0;
    if(atomic_compare_exchange_strong_explicit(&a733_npu_application_failed,&expected,
        reason ? reason : 1,memory_order_release,memory_order_relaxed))
        sunstone_crumb_fail(SUNSTONE_CRUMB_FAIL_SITE(NPU)|SUNSTONE_CRUMB_FAIL_RETURN,
            reason,__builtin_return_address(0));
}
static _Noreturn void fatal(void *unused)
{
    (void)unused;a733_npu_application_fail(1);
    if(xTaskGetSchedulerState()==taskSCHEDULER_RUNNING)vTaskSuspend(NULL);
    platform_panic("NPU application retained",0);
}
static void runtime_fatal(void *unused){fatal(unused);}
unsigned a733_npu_application_health(void)
{
    if(!app.used)return 0;
    if(atomic_load_explicit(&app.runtime.failed,memory_order_acquire) ||
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
    static const struct a733_mapping_rule readable={A733_MAPPING_READ,1,{{A733_MAIR_NORMAL_WB,A733_SH_INNER}},0,1},
        writable={A733_MAPPING_WRITE,1,{{A733_MAIR_NORMAL_WB,A733_SH_INNER}},0,1};
    if(!p || !bytes || a<0x41000000 || a>=limit || bytes>limit-a)return 0;
    return a733_mapping_check_pages(aarch64_device_translate,a,a,bytes,write?&writable:&readable);
}
/* The runtime still holds the linked parent region it was prepared with. */
static int parent(void)
{
    const struct npu_runtime *rt=&app.runtime;
    return rt->prepared && rt->parent_cpu==(uintptr_t)__npu_runtime_parent_start &&
        rt->parent_bytes && rt->parent_bytes<=(size_t)(__npu_runtime_parent_end-__npu_runtime_parent_start);
}
static unsigned slot_state(void)
{return atomic_load_explicit(&app.runtime.slot[0].state,memory_order_acquire);}

/* The GPU/PCK resource manager: its time, register and rail callbacks use the
 * image's counter and register lock, and a failed update retains the lock. */
static void retain_owned(void)
{
    TaskHandle_t self=xTaskGetCurrentTaskHandle();
    for(unsigned i=0;i<2;++i)
        if(xSemaphoreGetMutexHolder(app.locks.mutex[i])==self)
            (void)a733_platform_lock_retain(&app.locks,(enum a733_platform_lock)i);
}
static int gpu_now(void *v,uint64_t *out)
{return v==&app && !atomic_load_explicit(&app.locks.retained,memory_order_acquire) && a733_counter_clock_now(&app.clock,out);}
static int gpu_read(void *v,uintptr_t address,uint32_t *out)
{return v==&app && app.gpu_native.operations.read32(&app.gpu_native,address,out);}
static int gpu_update(void *v,uintptr_t address,uint32_t required,uint32_t set,uint64_t deadline)
{
    int ok=v==&app && app.gpu_native.operations.update32(&app.gpu_native,address,required,set,deadline);
    if(!ok)retain_owned();
    return ok;
}
static int gpu_rail(void *v,struct a733_gpu_resource_rail *out,uint64_t deadline)
{
    int ok=v==&app && app.gpu_native.operations.rail(&app.gpu_native,out,deadline);
    if(!ok)retain_owned();
    return ok;
}
static int gpu_manager_init(void)
{
    if(!a733_gpu_resources_native_init(&app.gpu_native,&app.locks.callbacks))return 0;
    const struct a733_gpu_resource_ops ops={gpu_now,gpu_read,gpu_update,gpu_rail};
    if(!a733_gpu_resources_init(&app.gpu,&ops,&app))return 0;
    app.gpu_native.manager=&app.gpu;app.gpu_native.manager_operations=ops;app.gpu_native.manager_context=&app;
    return 1;
}
static int pin_enter(void *cookie,uint64_t deadline)
{
    return cookie==&app && live() && xTaskGetCurrentTaskHandle()==app.pmic_task &&
        a733_platform_lock_enter(&app.locks,A733_PLATFORM_REGISTER_LOCK,deadline);
}
static void pin_leave(void *cookie)
{if(cookie!=&app)fatal(NULL);a733_platform_lock_leave(&app.locks,A733_PLATFORM_REGISTER_LOCK);}
static int pin_now(void *cookie,uint64_t *out)
{return cookie==&app && live() && a733_counter_clock_now(&app.clock,out);}

/* The power service task: every PMIC transaction and the GPU's domain 5. It
 * blocks until a submit, row request or the quiescence stop notifies it, or
 * for the ticks the step returns. */
static void pmic_task(void *argument)
{
    if(argument!=&app)fatal(NULL);
    for(;;) {
        if(atomic_load_explicit(&app.pmic_stop,memory_order_acquire)) {
            unsigned idle=a733_power_service_idle(&app.domains) &&
                !app.pmic.held && !app.pmic.retained && !app.pmic.failed &&
                !atomic_load_explicit(&app.pmic.busy,memory_order_acquire) &&
                app.pmic.active==A733_PMIC_REQUESTS;
            for(unsigned i=0;i<A733_PMIC_REQUESTS;++i)
                if(app.pmic.slots[i].state || app.pmic.slots[i].borrow.id)idle=0;
            if(idle) {
                atomic_store_explicit(&app.pmic_parked,1,memory_order_release);
                for(;;)vTaskSuspend(NULL);
            }
        }
        if(!live())break;
        const uint32_t ticks=a733_power_service_step(&app.domains);
        if(a733_power_service_ready(&app.domains)<0)break;
        (void)ulTaskNotifyTake(pdTRUE,(TickType_t)ticks);
    }
    a733_npu_application_fail(3);
    vTaskSuspend(NULL);fatal(NULL);
}
/* Startup, activation, then the local stop once the client's slot is CLOSED. */
static void bootstrap(void *argument)
{
    if(argument!=&app)fatal(NULL);
    for(;;) {
        uint64_t now;
        if(!NPU_APPLICATION_CHECK(live(),app.used) ||
           !NPU_APPLICATION_CHECK(parent(),app.used) ||
           !NPU_APPLICATION_CHECK(a733_counter_clock_now(&app.clock,&now),atomic_load_explicit(&app.clock.failed,memory_order_acquire)) ||
           !NPU_APPLICATION_CHECK(now<app.startup.config.deadline_us,now-app.startup.config.deadline_us))break;
        enum a733_npu_platform_result r=npu_runtime_startup_poll(&app.runtime);
        if(!NPU_APPLICATION_CHECK(r!=A733_NPU_PLATFORM_RETAINED,app.startup.stage))break;
        if(r==A733_NPU_PLATFORM_READY) {
            /* The worker may run inside activate(); the client starts after it. */
            enum npu_service_result a=npu_service_activate(&app.runtime.service);
            if(a==NPU_SERVICE_OK) {
                atomic_store_explicit(&app.ready,1,memory_order_release);
                for(;;) {
                    if(!NPU_APPLICATION_CHECK(live(),app.used) ||
                       !NPU_APPLICATION_CHECK(parent(),app.used) ||
                       !NPU_APPLICATION_CHECK(a733_counter_clock_now(&app.clock,&now),atomic_load_explicit(&app.clock.failed,memory_order_acquire)) ||
                       !NPU_APPLICATION_CHECK(now<app.startup.config.deadline_us,now-app.startup.config.deadline_us))break;
                    if(slot_state()==NPU_RUNTIME_CLOSED) {
                        enum a733_npu_platform_result stopped=npu_runtime_stop_poll(&app.runtime);
                        if(!NPU_APPLICATION_CHECK(stopped!=A733_NPU_PLATFORM_RETAINED,app.startup.stage))break;
                        if(stopped==A733_NPU_PLATFORM_READY) {
                            atomic_store_explicit(&app.stopped,1,memory_order_release);
                            vTaskSuspend(NULL);fatal(NULL);
                        }
                    }
                    vTaskDelay(1);
                }
                break;
            }
            if(!NPU_APPLICATION_CHECK(a==NPU_SERVICE_BUSY,a))break;
        }
        /* A PENDING startup blocks until its power-service notification, its
         * deadline or the last tick of its settle. */
        (void)ulTaskNotifyTake(pdTRUE,r==A733_NPU_PLATFORM_PENDING ? a733_npu_startup_wait_ticks(&app.startup) : 1);
    }
    a733_npu_application_fail(4);vTaskSuspend(NULL);fatal(NULL);
}
unsigned a733_npu_application_ready(void)
{return atomic_load_explicit(&app.ready,memory_order_acquire) && live();}
unsigned a733_npu_application_stopped(void)
{return atomic_load_explicit(&app.stopped,memory_order_acquire) && live();}
static int finite_irq_quiet(void)
{
    if(!npu_service_irq_stopped(&app.runtime.service))return 0;
    /* NPU INTID97: distributor bank3, bit1. Only observe the established
     * masked interrupt; never acknowledge or change inherited GIC state. */
    const uintptr_t addresses[]={0x03400000,0x0340010c,0x0340020c,0x0340030c};
    uint32_t value[4];
    static const struct a733_mapping_rule device={A733_MAPPING_READ,1,{{A733_MAIR_DEVICE_NGNRNE,A733_SH_ANY}},0,1};
    for(unsigned i=0;i<4;++i) {
        if(!a733_mapping_check(aarch64_device_translate,addresses[i],addresses[i],&device))return -1;
        value[i]=a733_mmio_read32_dsb_sy_both(addresses[i]);
    }
    return !(value[0]&UINT32_C(0x80000000)) && !((value[1]|value[2]|value[3])&2);
}
unsigned a733_npu_application_quiesce(void)
{
    const TaskHandle_t worker=app.runtime.service.worker;
    if(xTaskGetSchedulerState()!=taskSCHEDULER_RUNNING ||
       xTaskGetCurrentTaskHandle()!=app.client)return 0;
    if(!a733_npu_application_stopped() || !parent() || slot_state()!=NPU_RUNTIME_CLOSED ||
       app.runtime.service.controller.state!=A733_NPU_CLOSED ||
       !worker || !app.bootstrap_task || !app.pmic_task ||
       worker==app.client || app.bootstrap_task==app.client || app.pmic_task==app.client ||
       worker==app.pmic_task || worker==app.bootstrap_task || app.bootstrap_task==app.pmic_task)goto failed;
    /* CLOSED has no further register callbacks. On this single CPU, suspend
     * the runtime worker and bootstrap before checking provider storage. */
    vTaskSuspend(worker);
    vTaskSuspend(app.bootstrap_task);
    atomic_store_explicit(&app.pmic_stop,1,memory_order_release);
    xTaskNotifyGive(app.pmic_task); /* The service blocks until notified. */
    while(!atomic_load_explicit(&app.pmic_parked,memory_order_acquire)) {
        uint64_t now;
        if(!live() || !a733_counter_clock_now(&app.clock,&now) || now>=app.startup.config.deadline_us)goto failed;
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
           now>=app.startup.config.deadline_us)goto failed;
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
struct npu_runtime *a733_npu_application_runtime(void)
{
    return app.used && xTaskGetSchedulerState()==taskSCHEDULER_RUNNING &&
        xTaskGetCurrentTaskHandle()==app.client && a733_npu_application_ready() ? &app.runtime : NULL;
}
/* This application's own arenas: the sizing call's work memory and the EL1 metadata of its one runtime. */
static _Alignas(64) uint8_t sizing_scratch[65536], meta_memory[262144];
static struct npu_meta_window meta_window;
static int runtime_prepare(const struct npu_runtime_model *models,unsigned count)
{
    static const struct a733_mapping_rule wb={A733_MAPPING_WRITE,1,{{A733_MAIR_NORMAL_WB,A733_SH_INNER}},0,1};
    const uintptr_t start=(uintptr_t)__npu_runtime_parent_start,end=(uintptr_t)__npu_runtime_parent_end;
    if(!models || !count || (start|end)&(NPU_RUNTIME_GROUP_BYTES-1) || end<=start)return 0;
    for(unsigned m=0;m<count;++m)if(!image(models[m].data,models[m].bytes,0))return 0;
    const unsigned resident=a733_npu_runtime_resident_slots();
    const unsigned slots=resident?resident:1;
    const struct npu_runtime_bounds sizing={.scratch=sizing_scratch,.scratch_bytes=sizeof sizing_scratch};
    const size_t need=npu_runtime_requirements_with(models,count,slots,&sizing);
    npu_meta_window_init(&meta_window,meta_memory,sizeof meta_memory);
    if(!need || need>end-start ||
       !a733_mapping_check_stride(aarch64_device_translate,start,start,need,NPU_RUNTIME_GROUP_BYTES,&wb))return 0;
    struct npu_runtime_config config={.models=models,.count=count,.slots=slots,.cpu=start,.physical=start,.bytes=need,
        .register_cpu=0x03600000,.register_physical=0x03600000,.fatal=runtime_fatal,.meta=&meta_window.meta};
    return npu_runtime_prepare(&app.runtime,&config)&&
        (!resident||a733_npu_runtime_resident_prepare(&app.runtime));
}
int a733_npu_application_runtime_init(TaskHandle_t client,const struct npu_runtime_model *models,unsigned count)
{
    const struct a733_npu_application_boot *b=a733_npu_application_boot_record();
    if(!b)return 0;
    const struct a733_npu_application_profile *p=&b->profile;
    if(app.used || !client || xTaskGetSchedulerState()!=taskSCHEDULER_NOT_STARTED ||
       !preparation_admitted() || !a733_npu_application_observer_admit() ||
       !image(&app,sizeof(app),1) || !image(b,sizeof(*b),0) ||
       !p->npu_owner || !p->pmic_owner || !p->usb_owner || !p->bridge_owner ||
       p->npu_owner==p->pmic_owner || p->npu_owner==p->usb_owner || p->pmic_owner==p->usb_owner ||
       p->bridge_owner==p->npu_owner || p->bridge_owner==p->pmic_owner || p->bridge_owner==p->usb_owner ||
       !p->command_admission || b->pmic.owner!=p->pmic_owner || b->pmic.identity!=p->pmic_evidence || b->pmic.startup_deadline)return 0;
    app.used=1;app.client=client;
    uint64_t deadline=a733_observe_identity.deadline/24;
    if(!a733_counter_clock_init(&app.clock,24000000) ||
       !a733_clock_leases_init(&app.registry,a733_counter_clock_now,&app.clock) ||
       !a733_platform_locks_init(&app.locks,a733_counter_clock_now,&app.clock) ||
       !gpu_manager_init())goto failed;
    const struct a733_pins_locks pin_locks={pin_enter,pin_leave,pin_now,&app};
    const struct a733_rail_binding rail_binding={&app.registry,&app.gpu,
        p->bridge_owner,a733_counter_clock_now,&app.clock};
    if(!a733_pins_native_init(&app.pin_native,&pin_locks) ||
       !a733_pins_init_banks(&app.pins,&app.pin_native.operations,&app.pin_native,&b->pins) ||
       !a733_rail_leases_init(&app.rails,&rail_binding) || !runtime_prepare(models,count))goto failed;
    /* The power service runs below the bootstrap, on A55 core 0. */
    app.pmic_task=xTaskCreateStaticAffinitySet(pmic_task,"npu-pmic",4096,&app,3,app.stack[0],&app.task[0],1);
    app.bootstrap_task=xTaskCreateStaticAffinitySet(bootstrap,"npu-boot",4096,&app,4,app.stack[1],&app.task[1],1);
    if(!app.pmic_task || !app.bootstrap_task)goto failed;
    app.pmic_config=b->pmic;app.pmic_config.startup_deadline=deadline;
    if(!a733_pmic_native_init(&app.pmic_native,&app.locks,app.pmic_task) ||
       !a733_pmic_native_bind(&app.pmic,&app.pmic_config,&app.pmic_native,
           &app.registry,&app.rails,&app.pins) ||
       a733_pmic_identity(&app.pmic,&app.pmic_lifetime)!=A733_PMIC_READY ||
       !a733_gpu_resources_native_use_pmic(&app.gpu_native,&app.gpu,&app.pmic))goto failed;
    const struct a733_power_service_config service={p->pmic_owner,deadline,1000000u/configTICK_RATE_HZ,0};
    if(!a733_power_domains_native_init(&app.domains_native,app.pmic_task) ||
       !a733_power_domains_init(&app.domains,&app.pmic,&app.domains_native.operations,&app.domains_native,&service))
        goto failed;
    /* The startup borrows the image's providers; the domain-4 row is
     * registered here, before the power service first runs. */
    const struct a733_npu_startup_config startup={b->npu,p->npu_owner,deadline,0};
    const struct a733_npu_startup_providers providers={&app.clock,&app.registry,&app.locks,
        &app.gpu,&app.rails,&app.domains};
    const TaskHandle_t clients[]={client};
    /* Each runtime request and reply waits at most 2 s; the SUBMIT deadline
     * and the OPEN bootstrap deadline are the client's, at most 1 s. */
    if(!a733_npu_startup_init(&app.startup,&startup,&providers,NULL,NULL) ||
       !a733_npu_startup_bind_task(&app.startup,app.bootstrap_task) ||
       !(a733_npu_runtime_resident_slots()?
         npu_runtime_bind_resident(&app.runtime,&app.startup,client,4,1):
         npu_runtime_bind(&app.runtime,&app.startup,clients,4,1)) ||
       !npu_finite_runtime_irq_bind(&app.runtime) ||
       !npu_job_attach(&app.runtime,pdMS_TO_TICKS(2000)) ||
       !preparation_admitted())goto failed;
    return 1;
failed:
    a733_npu_application_fail(5);return 0;
}
