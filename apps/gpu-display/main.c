#include "application.h"
#include "cached.h"
#include "platform.h"
#include <string.h>
struct gpu_display_application app;
int hdmi_live(void *unused)
{
    (void)unused;
    if(atomic_load_explicit(&app.fatal,memory_order_acquire))return 0;
    unsigned expected=0,manager_failed=0;
    if(app.pck.initialized && atomic_compare_exchange_strong_explicit(&app.pck.busy,&expected,1,
       memory_order_acquire,memory_order_relaxed)){
        manager_failed=app.pck.failed;atomic_store_explicit(&app.pck.busy,0,memory_order_release);
    }
    if(manager_failed || atomic_load_explicit(&app.counter.failed,memory_order_acquire) ||
       atomic_load_explicit(&app.clocks.failed,memory_order_acquire) ||
       atomic_load_explicit(&app.locks.retained,memory_order_acquire) ||
       a733_smp_counter()/24>=app.deadline){hdmi_fail(NULL);return 0;}
    return 1;
}
void hdmi_fail(void *unused)
{(void)unused;atomic_store_explicit(&app.fatal,1,memory_order_release);}
void vApplicationStackOverflowHook(TaskHandle_t task,char *name)
{(void)task;platform_panic(name,2);}
static int now(void *v,uint64_t *out)
{return v==&app&&hdmi_live(NULL)&&a733_counter_clock_now(&app.counter,out)==1;}
static int pck_now(void *v,uint64_t *out)
{return v==&app.pck_native&&hdmi_live(NULL)&&a733_counter_clock_now(&app.counter,out)==1;}
static int enter(void *v,uint64_t deadline)
{return v==&app&&hdmi_live(NULL)?a733_platform_lock_enter(&app.locks,A733_PLATFORM_REGISTER_LOCK,deadline):-1;}
static void leave(void *v)
{configASSERT(v==&app);a733_platform_lock_leave(&app.locks,A733_PLATFORM_REGISTER_LOCK);}
static void pmic_task(void *unused)
{
    (void)unused;
    for(;;){
        if(atomic_load_explicit(&app.pmic_park_request,memory_order_acquire)){
            unsigned expected=0;
            if(atomic_compare_exchange_strong_explicit(&app.pmic.busy,&expected,1,
               memory_order_acquire,memory_order_relaxed)){
                unsigned failed=app.pmic.failed||app.pmic.retained;
                unsigned idle=!failed&&!app.pmic.held&&app.pmic.active==A733_PMIC_REQUESTS;
                atomic_store_explicit(&app.pmic.busy,0,memory_order_release);
                if(failed)hdmi_fail(NULL);
                if(idle){
                    /* GPU and HDMI consumers are stopped before this request.
                     * After publication this owner performs no provider call. */
                    atomic_store_explicit(&app.pmic_parked,1,memory_order_release);
                    for(;;)vTaskSuspend(NULL);
                }
            }
        }
        /* Let an already borrowed native PMIC transaction observe revocation
         * and retain its lock before this permanent owner parks. */
        enum a733_pmic_status r=a733_pmic_poll(&app.pmic);
        if(!hdmi_live(NULL))for(;;)vTaskSuspend(NULL);
        if(r==A733_PMIC_READY||r==A733_PMIC_COMPLETE)
            atomic_store_explicit(&app.pmic_ready,1,memory_order_release);
        else if(r!=A733_PMIC_PENDING&&r!=A733_PMIC_BUSY)hdmi_fail(NULL);
        vTaskDelay(1);
    }
}
BaseType_t gpu_display_task_privileged(TaskFunction_t entry,void *context)
{return pvr_gpu_service_task_privileged(entry,context)||(!context&&
    (entry==pmic_task||entry==gpu_display_output_task||entry==gpu_display_render_task))?pdTRUE:pdFALSE;}
void a733_smp_diagnostic_start(void)
{
    if(!a733_smp_device_preparation_admitted())platform_panic("GPU display preparation",1);
    const struct hdmi_session *source=gpu_display_hdmi_get();
    if(!source || memcmp(source->image,a733_smp_build_id,32) ||
       source->epoch!=a733_cached_runtime[0].epoch || source->pck_client==7 ||
       source->owner==9||source->owner==10||source->owner==11 ||
       !gpu_display_inputs_valid(gpu_display_inputs_get()))platform_panic("GPU display session",2);
    app.session=*source;app.deadline=a733_cached_runtime[0].diagnostic_deadline/24;
    if(app.deadline<=GPU_DISPLAY_STOP_RESERVE || a733_smp_counter()/24>=app.deadline-GPU_DISPLAY_STOP_RESERVE)
        platform_panic("GPU display deadline",3);
    app.stop_at=app.deadline-GPU_DISPLAY_STOP_RESERVE;
    if(!a733_counter_clock_init(&app.counter,24000000) ||
       !a733_platform_locks_init(&app.locks,a733_counter_clock_now,&app.counter) ||
       !a733_clock_leases_init(&app.clocks,a733_counter_clock_now,&app.counter))platform_panic("GPU display clock",4);
    struct a733_pins_locks pins={enter,leave,now,&app};
    if(!a733_pins_native_init(&app.pin_native,&pins) ||
       !a733_pins_init_banks(&app.pins,&app.pin_native.operations,&app.pin_native,&app.session.pins) ||
       !a733_gpu_resources_native_init(&app.pck_native,&app.locks.callbacks))platform_panic("GPU display providers",5);
    app.pck_native.operations.now=pck_now;
    if(!a733_gpu_resources_init(&app.pck,&app.pck_native.operations,&app.pck_native))platform_panic("GPU display PCK",6);
    struct a733_rail_binding rails={&app.clocks,&app.pck,app.session.generation,a733_counter_clock_now,&app.counter};
    if(!a733_rail_leases_init(&app.rails,&rails))platform_panic("GPU display rails",7);
    TaskFunction_t functions[]={pmic_task,gpu_display_output_task,gpu_display_render_task};
    const char *names[]={"gpu-display-pmic","gpu-display","gpu-render"};
    for(unsigned i=0;i<3;++i){
        app.handles[i]=xTaskCreateStatic(functions[i],names[i],4096,NULL,i==2?2:4,app.stacks[i],&app.tasks[i]);
        if(!app.handles[i])platform_panic("GPU display tasks",8);
    }
    struct a733_pmic_config pmic=app.session.pmic;pmic.startup_deadline=app.deadline;
    if(!a733_pmic_native_init(&app.pmic_native,&app.locks,app.handles[0]) ||
       !a733_pmic_init(&app.pmic,&pmic,&app.pmic_native.operations,&app.pmic_native,&app.clocks,&app.rails,&app.pins) ||
       !a733_gpu_resources_native_use_pmic(&app.pck_native,&app.pck,&app.pmic))platform_panic("GPU display PMIC",9);
    framebuffer.self=&framebuffer;framebuffer.session=app.session.framebuffer;
    struct a733_hdmi_native_config hdmi={.clocks=&app.clocks,.rails=&app.rails,.pmic=&app.pmic,
        .locks=&app.locks,.pck=&app.pck,.pck_client=app.session.pck_client,.task=app.handles[1],
        .owner=app.session.owner,.epoch=app.session.epoch,.request=app.session.request,
        .supervision=app.session.supervisor,.recovery_mode=A733_HDMI_SUPERVISED_FRAMEBUFFER,
        .pixel_source=hdmi_framebuffer_source,.pixel_context=&framebuffer};
    if(!a733_hdmi_native_init(&app.hdmi,&hdmi) || !gpu_display_render_prepare() ||
       !hdmi_framebuffer_boot())platform_panic("GPU display native binding",10);
    if(!a733_smp_device_preparation_admitted())platform_panic("GPU display startup expired",11);
    vTaskStartScheduler();platform_panic("GPU display scheduler returned",12);
}
