#include "application.h"
#include "snapshot.h"
#include "cached.h"
#include "platform.h"
#include <stddef.h>
#include <string.h>
struct hdmi_application app;
const uint64_t a733_hdmi_session_bytes=sizeof(struct hdmi_session);
const uint64_t a733_hdmi_framebuffer_mode=SUNSTONE_HDMI_FRAMEBUFFER;
#if SUNSTONE_HDMI_AUDIO_USB
const uint64_t a733_hdmi_audio_geometry[12]={2,816,440,216,HDMI_AUDIO_PCM_ADDRESS,192000,48000,2,16,32,656,160};
#elif SUNSTONE_HDMI_AUDIO
const uint64_t a733_hdmi_audio_geometry[10]={1,656,440,216,HDMI_AUDIO_PCM_ADDRESS,192000,48000,2,16,32};
#endif
#if SUNSTONE_HDMI_FRAMEBUFFER
const uint64_t a733_hdmi_framebuffer_offset=offsetof(struct hdmi_session,framebuffer);
#endif
int hdmi_live(void *unused)
{(void)unused;return !atomic_load_explicit(&app.fatal,memory_order_acquire)&&a733_smp_counter()/24<app.deadline;}
void hdmi_fail(void *unused)
{(void)unused;atomic_store_explicit(&app.fatal,1,memory_order_release);}
void a733_storage_maintenance_stop(void){hdmi_fail(NULL);}
void vApplicationStackOverflowHook(TaskHandle_t task,char *name)
{(void)task;platform_panic(name,2);}
static int now(void *v,uint64_t *out)
{return v==&app&&a733_counter_clock_now(&app.counter,out)==1;}
static int pck_now(void *v,uint64_t *out)
{return v==&app.pck_native&&a733_counter_clock_now(&app.counter,out)==1;}
static int enter(void *v,uint64_t deadline)
{return v==&app&&hdmi_live(NULL)?a733_platform_lock_enter(&app.locks,A733_PLATFORM_REGISTER_LOCK,deadline):-1;}
static void leave(void *v)
{configASSERT(v==&app);a733_platform_lock_leave(&app.locks,A733_PLATFORM_REGISTER_LOCK);}
static void pmic_task(void *unused)
{
    (void)unused;
    for(;;){
        if(hdmi_live(NULL)){
            enum a733_pmic_status r=a733_pmic_poll(&app.pmic);
            if(r==A733_PMIC_READY||r==A733_PMIC_COMPLETE)
                atomic_store_explicit(&app.pmic_ready,1,memory_order_release);
            else if(r!=A733_PMIC_PENDING&&r!=A733_PMIC_BUSY)hdmi_fail(NULL);
        }
#if SUNSTONE_HDMI_AUDIO
        if(app.pmic.held){taskYIELD();continue;}
#endif
        vTaskDelay(1);
    }
}
int a733_storage_native_task_privileged(void (*entry)(void *),void *context)
{
#if SUNSTONE_HDMI_AUDIO
    if(dma_service_task_privileged(entry,context))return 1;
#endif
    return !context&&(entry==pmic_task||entry==hdmi_output_task
#if SUNSTONE_HDMI_AUDIO_USB
        ||entry==hdmi_audio_usb_task
#endif
        );
}
void a733_smp_diagnostic_start(void)
{
    if(!a733_smp_device_preparation_admitted())platform_panic("HDMI startup",1);
    const struct hdmi_session *source=hdmi_admitted_session();
    if(!source)platform_panic("HDMI sealed session",2);
    app.session=*source;
    if(!hdmi_session_valid(&app.session)||memcmp(app.session.image,a733_smp_build_id,32)||
       app.session.epoch!=a733_cached_runtime[0].epoch)platform_panic("HDMI session",2);
    app.deadline=a733_cached_runtime[0].diagnostic_deadline/24;
    if(app.deadline<=2000000||a733_smp_counter()/24>=app.deadline-2000000)
        platform_panic("HDMI time",3);
    app.stop_at=app.deadline-2000000;
    if(!a733_counter_clock_init(&app.counter,24000000)||
       !a733_platform_locks_init(&app.locks,a733_counter_clock_now,&app.counter)||
       !a733_clock_leases_init(&app.clocks,a733_counter_clock_now,&app.counter))
        platform_panic("HDMI clock",4);
    struct a733_pins_locks pl={enter,leave,now,&app};
    if(!a733_pins_native_init(&app.pin_native,&pl)||
       !a733_pins_init_banks(&app.pins,&app.pin_native.operations,&app.pin_native,&app.session.pins)||
       !a733_gpu_resources_native_init(&app.pck_native,&app.locks.callbacks))
        platform_panic("HDMI resources",5);
    app.pck_native.operations.now=pck_now;
    if(!a733_gpu_resources_init(&app.pck,&app.pck_native.operations,&app.pck_native))
        platform_panic("HDMI PCK",6);
    struct a733_rail_binding rb={&app.clocks,&app.pck,app.session.generation,a733_counter_clock_now,&app.counter};
    if(!a733_rail_leases_init(&app.rails,&rb))platform_panic("HDMI rails",7);
    TaskFunction_t functions[]={pmic_task,hdmi_output_task
#if SUNSTONE_HDMI_AUDIO_USB
        ,hdmi_audio_usb_task
#endif
    };
    const char *names[]={"hdmi-pmic","hdmi-output"
#if SUNSTONE_HDMI_AUDIO_USB
        ,"hdmi-audio-usb"
#endif
    };
    for(unsigned i=0;i<HDMI_TASK_COUNT;++i){
        app.handles[i]=xTaskCreateStatic(functions[i],names[i],2048,NULL,4,app.stacks[i],&app.tasks[i]);
        if(!app.handles[i])platform_panic("HDMI tasks",8);
    }
    struct a733_pmic_config pmic=app.session.pmic;pmic.startup_deadline=app.deadline;
    if(!a733_pmic_native_init(&app.pmic_native,&app.locks,app.handles[0])||
       !a733_pmic_init(&app.pmic,&pmic,&app.pmic_native.operations,&app.pmic_native,
          &app.clocks,&app.rails,&app.pins)||
       !a733_gpu_resources_native_use_pmic(&app.pck_native,&app.pck,&app.pmic))
        platform_panic("HDMI PMIC",9);
    struct a733_hdmi_native_config config={.clocks=&app.clocks,.rails=&app.rails,
        .pmic=&app.pmic,.locks=&app.locks,.pck=&app.pck,.pck_client=app.session.pck_client,
        .task=app.handles[1],.owner=app.session.owner,.epoch=app.session.epoch,
        .request=app.session.request,.supervision=app.session.supervisor,
#if SUNSTONE_HDMI_FRAMEBUFFER
        .recovery_mode=A733_HDMI_SUPERVISED_FRAMEBUFFER,
        .pixel_source=a733_display_framebuffer_source,.pixel_context=&framebuffer
#else
        .recovery_mode=A733_HDMI_SUPERVISED_PATTERN
#endif
        };
    if(!a733_hdmi_native_init(&app.hdmi,&config))platform_panic("HDMI native",10);
#if SUNSTONE_HDMI_FRAMEBUFFER
    if(!hdmi_framebuffer_boot())platform_panic("HDMI framebuffer memory",12);
#endif
#if SUNSTONE_HDMI_AUDIO
    if(!hdmi_audio_boot())platform_panic("HDMI audio memory/provider",14);
#endif
#if SUNSTONE_HDMI_AUDIO_USB
    if(!hdmi_audio_usb_init())platform_panic("HDMI audio USB authority",15);
#endif
    if(!a733_smp_device_preparation_admitted())platform_panic("HDMI startup expired",13);
    vTaskStartScheduler();platform_panic("HDMI scheduler returned",11);
}
