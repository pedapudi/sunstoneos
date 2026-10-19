#include "application.h"
#include "session.h"
#include "a733_device_memory_layout.h"
#include "platform.h"
#include "a733_maintenance_watchdog_internal.h"
#include <string.h>
#ifdef SUNSTONE_GPU_FINITE_RETURN
#include "../gpu-finite/finite.h"
#if !defined(SUNSTONE_GPU_SINGLE_CORE) || !defined(SUNSTONE_GPU_SUPERVISED) || !defined(SUNSTONE_GPU_COMPUTE_APPLICATION) || defined(SUNSTONE_GPU_TRANSPORT_ONLY)
#error "Finite GPU adapter requires supervised single-core compute"
#endif
#endif
struct gpu_application app;
#ifdef SUNSTONE_GPU_TRANSPORT_ONLY
const uint64_t gpu_transport_only_marker[2]={UINT64_C(0x4750555553423031),1};
#endif
#ifdef SUNSTONE_GPU_RUNTIME_INPUT
const uint64_t gpu_runtime_input_marker[2]={UINT64_C(0x475055494e503032),2};
#endif
#ifdef SUNSTONE_GPU_RUNTIME_DRAW
const uint64_t gpu_runtime_draw_marker[2]={UINT64_C(0x4750554452415731),PVR_RENDER_DRAW_PROFILE};
#endif
#ifdef SUNSTONE_GPU_RUNTIME_TEXTURE
const uint64_t gpu_runtime_texture_marker[2]={UINT64_C(0x4750555458543031),PVR_RENDER_TEXTURE_PROFILE};
#endif
#ifdef SUNSTONE_GPU_PROGRAM_APPLICATION
const uint64_t gpu_program_application_marker[2]={UINT64_C(0x4750555052473031),1};
#endif
extern const unsigned char gpu_firmware[],gpu_firmware_end[];
static _Noreturn void park(void){for(;;)vTaskDelay(1);}
int gpu_application_live(void *unused)
{(void)unused;return !atomic_load_explicit(&app.fatal,memory_order_acquire)&&
    a733_smp_counter()/24<app.session.deadline_us;}
void gpu_application_fail(void *unused)
{(void)unused;atomic_store_explicit(&app.fatal,1,memory_order_release);
 atomic_store_explicit(&app.result,2,memory_order_release);}
void a733_storage_maintenance_stop(void)
{gpu_application_fail(NULL);}
void vApplicationStackOverflowHook(TaskHandle_t task,char *name)
{(void)task;platform_panic(name,2);}
static void pmic(void *unused)
{
    (void)unused;for(;;){
#ifdef SUNSTONE_GPU_FINITE_RETURN
        gpu_finite_pmic_checkpoint();
#endif
        /* A provider must poll once after a consumer fault to retain its mutex. */
        enum a733_pmic_status r=a733_pmic_poll(&app.shared.pmic);
        if(!gpu_application_live(NULL))park();
        if(r==A733_PMIC_READY||r==A733_PMIC_COMPLETE)
            atomic_store_explicit(&app.shared.pmic_ready,1,memory_order_release);
        else if(r!=A733_PMIC_PENDING&&r!=A733_PMIC_BUSY){gpu_application_fail(NULL);park();}
        vTaskDelay(1);
    }
}
static _Noreturn void retained(void)
{gpu_application_fail(NULL);park();}
static struct pvr_gpu_service_reply request(enum pvr_gpu_service_operation op,uint64_t timeout)
{
    struct pvr_gpu_service_request q={op,timeout};struct pvr_gpu_service_reply reply;uint64_t ticket;
    if(!gpu_application_live(NULL)||!pvr_gpu_service_send(&app.service,&q,1,&ticket)){
        retained();}
    while(!pvr_gpu_service_collect(&app.service,ticket,&reply,1))
        if(!gpu_application_live(NULL)){retained();}
    app.last=reply;
    if(reply.result!=PVR_GPU_SERVICE_OK){retained();}
    return reply;
}
#ifdef SUNSTONE_GPU_RUNTIME_INPUT
static void runtime_inputs(void)
{
    atomic_store_explicit(&app.runtime_sequence,1,memory_order_release);
    atomic_store_explicit(&app.result,1,memory_order_release);
    for(;;) {
        while(atomic_load_explicit(&app.runtime_state,memory_order_acquire)!=1) {
            if(!gpu_application_live(NULL))retained();
            (void)request(PVR_GPU_SERVICE_STATUS,0);vTaskDelay(1);
        }
        if(app.runtime_operation==3)break;
        uint64_t ticket;struct pvr_gpu_service_reply r;
        if(!pvr_gpu_service_compute_input(&app.service,app.runtime_expected,app.runtime_input,
            app.runtime_bytes,1000000,1,&ticket)) {
            atomic_store_explicit(&app.runtime_state,0,memory_order_release);
            atomic_store_explicit(&app.result,1,memory_order_release);continue;
        }
        while(!pvr_gpu_service_collect(&app.service,ticket,&r,1))if(!gpu_application_live(NULL))retained();
        if(r.result==PVR_GPU_SERVICE_FAULT)retained();
        if(r.result==PVR_GPU_SERVICE_OK) {
            uint32_t sequence=r.compute_sequence;
            atomic_store_explicit(&app.runtime_sequence,sequence,memory_order_release);
            while(r.compute_phase!=PVR_COMPUTE_SERVICE_COMPLETE){vTaskDelay(1);r=request(PVR_GPU_SERVICE_STATUS,0);}
            unsigned total=gpu_application_package()->output_bytes/4;
            for(unsigned first=0;first<total;first+=32) {
                if(!pvr_gpu_service_compute_read_sequence(&app.service,sequence,first,1,&ticket))retained();
                while(!pvr_gpu_service_collect(&app.service,ticket,&r,1))if(!gpu_application_live(NULL))retained();
                unsigned n=total-first<32?total-first:32;
                if(r.result!=PVR_GPU_SERVICE_OK||r.compute_sequence!=sequence||r.compute_first_word!=first||
                   r.compute_word_count!=n||r.compute_total_words!=total)retained();
                memcpy(app.pixels+first,r.compute_words,n*4);
            }
        }
        atomic_store_explicit(&app.runtime_state,0,memory_order_release);
        atomic_store_explicit(&app.result,1,memory_order_release);
    }
}
#endif
#if defined(SUNSTONE_GPU_RUNTIME_DRAW) && !defined(SUNSTONE_GPU_RUNTIME_TEXTURE)
static void runtime_draw(void)
{
    for(;;) {
        while(atomic_load_explicit(&app.runtime_state,memory_order_acquire)!=1) {
            if(!gpu_application_live(NULL))retained();
            (void)request(PVR_GPU_SERVICE_STATUS,0);vTaskDelay(1);
        }
        if(app.runtime_operation==3)return;
        uint64_t ticket;struct pvr_gpu_service_reply r;
        int sent=pvr_gpu_service_draw_next(&app.service,app.last.epoch,app.runtime_expected,app.runtime_input,app.runtime_bytes,
            app.runtime_bytes/sizeof(struct pvr_render_vertex),1000000,1,&ticket);
        if(sent) {
            while(!pvr_gpu_service_collect(&app.service,ticket,&r,1))if(!gpu_application_live(NULL))retained();
            if(r.result==PVR_GPU_SERVICE_FAULT)retained();
            sent=r.result==PVR_GPU_SERVICE_OK;
        }
        if(sent) {
            uint32_t frame=r.render_sequence;
            if(frame!=app.runtime_expected+1)retained();
            atomic_store_explicit(&app.runtime_sequence,frame,memory_order_release);
            while(r.render_phase!=PVR_RENDER_SERVICE_COMPLETE){vTaskDelay(1);r=request(PVR_GPU_SERVICE_STATUS,0);}
            for(unsigned first=0;first<4096;first+=64) {
                if(!pvr_gpu_service_render_read_sequence(&app.service,frame,first,1,&ticket))retained();
                while(!pvr_gpu_service_collect(&app.service,ticket,&r,1))if(!gpu_application_live(NULL))retained();
                if(r.result!=PVR_GPU_SERVICE_OK || r.render_sequence!=frame || r.render_first_pixel!=first || r.render_pixel_count!=64)retained();
                memcpy(app.pixels+first,r.render_pixels,256);
            }
        }
        atomic_store_explicit(&app.result,sent || atomic_load_explicit(&app.runtime_sequence,memory_order_acquire)?1:4,memory_order_release);
        atomic_store_explicit(&app.runtime_state,0,memory_order_release);
    }
}
#endif
static void client(void *unused)
{
    (void)unused;
    if(atomic_load_explicit(&app.preparation_failed,memory_order_acquire))park();
    /* Do not let GPU OPEN revoke the shared providers before the host has
     * received one complete pending response from the initialized transport. */
    while(!atomic_load_explicit(&app.shared.pmic_ready,memory_order_acquire)||
#ifndef SUNSTONE_GPU_FINITE_RETURN
          !atomic_load_explicit(&app.usb_observed,memory_order_acquire)
#else
          0
#endif
          ){
        if(!gpu_application_live(NULL)){retained();}vTaskDelay(1);}
#ifdef SUNSTONE_GPU_FINITE_RETURN
    platform_puts("GPU_STAGE=OPEN\n");
#endif
#ifdef SUNSTONE_GPU_RUNTIME_TEXTURE
    /* Initial global list has 1024 pages, with separate allocate/zero/publish
     * steps and tick waits. Its OPEN budget must cover that bounded setup. */
    struct pvr_gpu_service_reply r=request(PVR_GPU_SERVICE_OPEN,15000000);
#else
    struct pvr_gpu_service_reply r=request(PVR_GPU_SERVICE_OPEN,1000000);
#endif
    while(r.phase!=PVR_GPU_SERVICE_READY){vTaskDelay(1);r=request(PVR_GPU_SERVICE_STATUS,0);}
#if defined(SUNSTONE_GPU_RUNTIME_TEXTURE)
    gpu_application_texture_client();
#elif defined(SUNSTONE_GPU_RUNTIME_DRAW)
    runtime_draw();
#elif defined(SUNSTONE_GPU_COMPUTE_APPLICATION)
#ifdef SUNSTONE_GPU_PROGRAM_APPLICATION
    unsigned total=gpu_application_package()->output_bytes/4;
    r=request(PVR_GPU_SERVICE_COMPUTE,1000000);
    while(r.compute_phase!=PVR_COMPUTE_SERVICE_COMPLETE){vTaskDelay(1);r=request(PVR_GPU_SERVICE_STATUS,0);}
    for(unsigned first=0;first<total;first+=32) {
        uint64_t ticket;
        if(!pvr_gpu_service_compute_read(&app.service,first,1,&ticket)){retained();}
        while(!pvr_gpu_service_collect(&app.service,ticket,&r,1))if(!gpu_application_live(NULL)){retained();}
        unsigned count=total-first<32?total-first:32;
        if(r.result!=PVR_GPU_SERVICE_OK||r.compute_first_word!=first||r.compute_word_count!=count||
           r.compute_total_words!=total||r.compute_sequence!=1){retained();}
        memcpy(app.pixels+first,r.compute_words,count*4);
    }
#else
    for(unsigned n=0;n<2;++n) {
        r=request(PVR_GPU_SERVICE_COMPUTE,1000000);
        while(r.compute_phase!=PVR_COMPUTE_SERVICE_COMPLETE) {
            vTaskDelay(1);r=request(PVR_GPU_SERVICE_STATUS,0);
        }
        r=request(PVR_GPU_SERVICE_COMPUTE_OUTPUT,0);
        if(r.compute_word_count!=PVR_COMPUTE_JOB_WORDS||r.compute_sequence!=n+1){retained();}
        memcpy(app.compute_words[n],r.compute_words,sizeof(r.compute_words));
        app.compute_sequence[n]=r.compute_sequence;
    }
#endif
#else
    r=request(PVR_GPU_SERVICE_RENDER,1000000);
    while(r.render_phase!=PVR_RENDER_SERVICE_COMPLETE){vTaskDelay(1);r=request(PVR_GPU_SERVICE_STATUS,0);}
    if(r.render_checked!=PVR_RENDER_OUTPUT_BYTES){retained();}
    for(unsigned first=0;first<4096;first+=64){
        uint64_t ticket;
        if(!gpu_application_live(NULL)||!pvr_gpu_service_render_read(&app.service,first,1,&ticket)){
            retained();}
        while(!pvr_gpu_service_collect(&app.service,ticket,&r,1))
            if(!gpu_application_live(NULL)){retained();}
        app.last=r;
        if(r.result!=PVR_GPU_SERVICE_OK||r.render_first_pixel!=first||r.render_pixel_count!=64){
            retained();}
        memcpy(app.pixels+first,r.render_pixels,sizeof(r.render_pixels));
    }
#endif
#ifdef SUNSTONE_GPU_RUNTIME_INPUT
    runtime_inputs();
#endif
    if(!gpu_application_live(NULL)){retained();}
#ifdef SUNSTONE_GPU_FINITE_RETURN
    platform_puts("GPU_STAGE=CLOSE\n");
#endif
    r=request(PVR_GPU_SERVICE_CLOSE,1000000);
    while(r.phase!=PVR_GPU_SERVICE_STOPPED){
        vTaskDelay(1);r=request(PVR_GPU_SERVICE_STATUS,0);
    }
#ifdef SUNSTONE_GPU_RUNTIME_TEXTURE
    gpu_application_texture_closed();
#endif
#ifdef SUNSTONE_GPU_FINITE_RETURN
    gpu_finite_complete();
#endif
#if defined(SUNSTONE_GPU_RUNTIME_INPUT) || defined(SUNSTONE_GPU_RUNTIME_DRAW)
    atomic_store_explicit(&app.runtime_closed,1,memory_order_release);
    atomic_store_explicit(&app.runtime_state,0,memory_order_release);
#endif
    atomic_store_explicit(&app.result,1,memory_order_release);
    /* Copied output and reserved parents stay owned for host capture. CLOSE
     * returned the GPU power token; USB/PMIC retain their own live session. */
    for(;;){vTaskDelay(1);(void)request(PVR_GPU_SERVICE_STATUS,0);}
}
int a733_storage_native_task_privileged(void (*entry)(void *),void *context)
{return pvr_gpu_service_task_privileged(entry,context)||(!context&&
    (entry==pmic||entry==gpu_application_usb||entry==client));}
void gpu_application_irq(void)
{
    if(atomic_load_explicit(&app.preparation_failed,memory_order_acquire))return;
    BaseType_t woken=pdFALSE;
    if(!pvr_gpu_service_interrupt(&app.service,&woken))gpu_application_fail(NULL);
    portYIELD_FROM_ISR(woken);
}
void a733_smp_diagnostic_start(void)
{
#ifdef SUNSTONE_GPU_RUNTIME_TEXTURE
    __asm__ volatile("" :: "r"(gpu_runtime_texture_marker) : "memory");
#endif
#ifdef SUNSTONE_GPU_RUNTIME_DRAW
    __asm__ volatile("" :: "r"(gpu_runtime_draw_marker) : "memory");
#endif
#ifdef SUNSTONE_GPU_PROGRAM_APPLICATION
    __asm__ volatile("" :: "r"(gpu_program_application_marker) : "memory");
#ifdef SUNSTONE_GPU_RUNTIME_INPUT
    __asm__ volatile("" :: "r"(gpu_runtime_input_marker) : "memory");
#endif
#endif
    if(!a733_smp_device_preparation_admitted()||!a733_device_memory_ready())platform_panic("GPU memory admission",1);
    const struct a733_board_session *sealed=gpu_application_session();
    if(!sealed)platform_panic("GPU immutable session",2);
    memcpy(&app.session,sealed,sizeof(app.session));
    struct a733_board_session *s=&app.session;
    if(!a733_board_session_valid(s,a733_smp_counter())||
#ifndef SUNSTONE_GPU_SUPERVISED
       s->workload||
#else
       s->workload!=2||
#endif
       s->epoch!=a733_cached_runtime[0].epoch||
#ifndef SUNSTONE_GPU_SUPERVISED
       s->deadline_us*24!=a733_cached_runtime[0].diagnostic_deadline
#else
       a733_cached_runtime[0].startup_deadline<A733_SMP_COUNTER_HZ
#endif
       )platform_panic("GPU session",2);
#ifdef SUNSTONE_GPU_SUPERVISED
    s->session_start_ticks=a733_cached_runtime[0].startup_deadline-A733_SMP_COUNTER_HZ;
    s->deadline_us=a733_cached_runtime[0].diagnostic_deadline/24;
#else
    struct a733_watchdog_window window={s->deadline_us,s->guard_us,s->rtc_control,s->rtc_divider,s->rtc_source};
    if(!a733_watchdog_hw_admit(&window))platform_panic("GPU watchdog",2);
#endif
    struct a733_native_usb_config b={.identity=s->generation};
    b.pmic.owner=s->pmic_owner;b.pmic.identity=s->generation;b.pmic.startup_deadline=s->deadline_us;
    for(unsigned i=0;i<2;++i)b.pmic.pins[i]=(struct a733_pin_config){s->pmic_pins[i][0],s->pmic_pins[i][1],s->pmic_pins[i][2]};
#ifdef SUNSTONE_GPU_SUPERVISED
    b.pins.authorized[A733_PIN_L]=3u;
#else
    b.pins.authorized[A733_PIN_L]=3u|(1u<<8)|(1u<<9)|(1u<<12)|(1u<<13);
#endif
    b.pins.latch[0]=(struct a733_pin_latch_handoff){s->latch_identity,s->latch_owner,s->latch_known_mask,s->latch_value};
    b.usb.owner=s->usb0_owner;b.usb.handoff=s->generation;b.usb.deadline_us=s->deadline_us;b.usb.cold_reference=s->usb0_cold_reference;
    memcpy(b.usb_supply_uv,s->usb_supply_uv,sizeof(b.usb_supply_uv));
    memcpy(b.usb.fabric,s->fabric,sizeof(b.usb.fabric));memcpy(b.usb.rate_hz,s->fabric_hz,sizeof(b.usb.rate_hz));
    if(!a733_native_usb_init(&app.shared,&b,gpu_application_live,gpu_application_fail,NULL))platform_panic("GPU shared providers",3);
    TaskFunction_t functions[]={pmic,gpu_application_usb,client};
    const char *names[]={"gpu-pmic","gpu-usb","gpu-client"};
    for(unsigned i=0;i<3;++i){
#ifdef SUNSTONE_GPU_FINITE_RETURN
        if(i==1)continue; /* No native USB task or controller preparation. */
#endif
#ifdef SUNSTONE_GPU_SINGLE_CORE
        app.handles[i]=xTaskCreateStatic(functions[i],names[i],4096,NULL,2,app.stacks[i],&app.tasks[i]);
#else
        app.handles[i]=xTaskCreateStaticAffinitySet(functions[i],names[i],4096,NULL,2,
        app.stacks[i],&app.tasks[i],1);
#endif
        if(!app.handles[i])platform_panic("GPU tasks",4);}
#ifdef SUNSTONE_GPU_FINITE_RETURN
    if(!gpu_finite_bind_pmic())platform_panic("GPU finite PMIC bind",5);
#else
    if(!a733_native_usb_bind(&app.shared,app.handles[0],app.handles[1]))platform_panic("GPU USB bind",5);
#endif
#ifdef SUNSTONE_GPU_TRANSPORT_ONLY
    /* Keep the real PMIC/USB tasks and pending-response protocol. The client
     * parks before OPEN; no GPU platform, firmware or worker is constructed. */
    __asm__ volatile("" :: "r"(gpu_transport_only_marker) : "memory");
    atomic_store_explicit(&app.preparation_failed,1,memory_order_release);
#else
    if(!a733_gpu_platform_native_init(&app.native,&app.shared.locks.callbacks)||
       !a733_gpu_platform_init(&app.power,&app.shared.gpu,7,&app.native.operations,&app.native))platform_panic("GPU platform",6);
    struct a733_pmic_lifetime lifetime;
    if(a733_pmic_identity(&app.shared.pmic,&lifetime)!=A733_PMIC_READY||
       !a733_gpu_platform_bind_pmic(&app.power,&app.shared.gpu_native,&app.shared.pmic,&lifetime,9))platform_panic("GPU supply",7);
#ifndef SUNSTONE_GPU_COMPUTE_APPLICATION
#ifdef SUNSTONE_GPU_RUNTIME_TEXTURE
    uint32_t width,height;
    if(!gpu_application_texture_geometry(&width,&height))platform_panic("GPU texture session",7);
    const struct a733_gpu_graphics_profile profile={{width,height,16384},1024,1024,25,5000000};
#else
    const struct a733_gpu_graphics_profile profile={{64,64,32},28,4,25,1000000};
#endif
#endif
    static const struct a733_gpu_backend_external external={.revoked=&app.fatal};
    if(!a733_gpu_reserved_prepare(&app.firmware,9,gpu_firmware,(size_t)(gpu_firmware_end-gpu_firmware),400000000)||
#ifdef SUNSTONE_GPU_PROGRAM_APPLICATION
       !a733_gpu_compute_reserved_prepare_package(&app.compute,&app.firmware,&app.service,10,gpu_application_package())||
#else
       !a733_gpu_compute_reserved_prepare(&app.compute,&app.firmware,&app.service,10)||
#endif
#ifndef SUNSTONE_GPU_COMPUTE_APPLICATION
#if defined(SUNSTONE_GPU_RUNTIME_TEXTURE)
       !a733_gpu_graphics_memory_prepare_texture(&app.graphics,&app.firmware,&app.compute,&app.service,11,&profile)||
#elif defined(SUNSTONE_GPU_RUNTIME_DRAW)
       !a733_gpu_graphics_memory_prepare_draw(&app.graphics,&app.firmware,&app.compute,&app.service,11,&profile)||
#else
       !a733_gpu_graphics_memory_prepare_clear(&app.graphics,&app.firmware,&app.compute,&app.service,11,&profile)||
#endif
#endif
       !a733_gpu_backend_bind(&app.backend,&app.service,&app.firmware.config,&app.firmware.memory,&app.power,&external)||
       !pvr_gpu_service_init(&app.service,&app.firmware.config,3,1)||
       !pvr_gpu_service_bind(&app.service,0,app.handles[2])){
        /* Preparation has not opened the GPU. Keep every allocation and
         * prevent a partially bound worker from running, while healthy
         * shared providers deliver the existing terminal failure report. */
        atomic_store_explicit(&app.preparation_failed,1,memory_order_release);
        atomic_store_explicit(&app.result,2,memory_order_release);
        if(app.service.worker)vTaskSuspend(app.service.worker);
    }
#endif
    vTaskStartScheduler();platform_panic("GPU scheduler returned",9);
}
