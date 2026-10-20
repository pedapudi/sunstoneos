#include "application.h"
#include "boot.h"
#include "platform.h"
#include <string.h>
extern const unsigned char gpu_firmware[],gpu_firmware_end[];
static _Noreturn void retained(void)
{hdmi_fail(NULL);for(;;)vTaskSuspend(NULL);}
static uint64_t budget(uint64_t maximum)
{
    uint64_t now;
    if(!hdmi_live(NULL) || !a733_counter_clock_now(&app.counter,&now) || now>=app.stop_at)retained();
    uint64_t remaining=app.stop_at-now;
    return remaining<maximum?remaining:maximum;
}
static struct pvr_gpu_service_reply lifecycle(enum pvr_gpu_service_operation op,uint64_t timeout)
{
    struct pvr_gpu_service_request q={op,timeout?budget(timeout):0};
    struct pvr_gpu_service_reply r;uint64_t ticket;
    if(!hdmi_live(NULL)||!pvr_gpu_service_send(&app.gpu_service,&q,1,&ticket))retained();
    while(!pvr_gpu_service_collect(&app.gpu_service,ticket,&r,1))(void)budget(1000000);
    if(r.result!=PVR_GPU_SERVICE_OK)retained();return r;
}
static struct pvr_gpu_service_reply scoped(uint64_t epoch,struct pvr_render_texture_request *q,
    const void *data,uint64_t timeout,uint64_t *ticket)
{
    struct pvr_gpu_service_reply r;
    if(!pvr_gpu_service_texture(&app.gpu_service,epoch,q,data,budget(timeout),1,ticket))retained();
    while(!pvr_gpu_service_collect(&app.gpu_service,*ticket,&r,1))(void)budget(1000000);
    if(r.result!=PVR_GPU_SERVICE_OK)retained();return r;
}
int gpu_display_render_prepare(void)
{
    if(!gpu_display_inputs_valid(gpu_display_inputs_get()) || !app.handles[2])return 0;
    struct a733_pmic_lifetime life;
    const struct a733_gpu_graphics_profile profile={{GPU_DISPLAY_WIDTH,GPU_DISPLAY_HEIGHT,16384},1024,1024,25,5000000};
    static const struct a733_gpu_backend_external external={.revoked=&app.fatal};
    return a733_gpu_platform_native_init(&app.gpu_native,&app.locks.callbacks) &&
        a733_gpu_platform_init(&app.gpu_power,&app.pck,7,&app.gpu_native.operations,&app.gpu_native) &&
        a733_pmic_identity(&app.pmic,&life)==A733_PMIC_READY &&
        a733_gpu_platform_bind_pmic(&app.gpu_power,&app.pck_native,&app.pmic,&life,9) &&
        a733_gpu_reserved_prepare(&app.gpu_firmware,9,gpu_firmware,(size_t)(gpu_firmware_end-gpu_firmware),400000000) &&
        a733_gpu_compute_reserved_prepare(&app.gpu_compute,&app.gpu_firmware,&app.gpu_service,10) &&
        a733_gpu_graphics_memory_prepare_texture(&app.gpu_graphics,&app.gpu_firmware,&app.gpu_compute,&app.gpu_service,11,&profile) &&
        a733_gpu_backend_bind(&app.gpu_backend,&app.gpu_service,&app.gpu_firmware.config,&app.gpu_firmware.memory,&app.gpu_power,&external) &&
        pvr_gpu_service_init(&app.gpu_service,&app.gpu_firmware.config,3,1) &&
        pvr_gpu_service_bind(&app.gpu_service,0,app.handles[2]);
}
void gpu_display_render_task(void *unused)
{
    (void)unused;
    if(xTaskGetCurrentTaskHandle()!=app.handles[2])retained();
    const struct gpu_display_inputs *inputs=gpu_display_inputs_get();
    if(!gpu_display_inputs_valid(inputs))retained();
    /* All USB supply snapshots finish before rendering. After USB publication
     * it performs metadata-only PMIC health checks, so terminal park cannot
     * strand a later USB power request. No host enumeration is awaited. */
    while(!atomic_load_explicit(&app.usb_ready,memory_order_acquire)){(void)budget(1000000);vTaskDelay(1);}
    struct pvr_gpu_service_reply r=lifecycle(PVR_GPU_SERVICE_OPEN,15000000);
    while(r.phase!=PVR_GPU_SERVICE_READY){vTaskDelay(1);r=lifecycle(PVR_GPU_SERVICE_STATUS,0);}
    uint64_t epoch=r.epoch;
    for(unsigned frame=0;frame<2;++frame){
        const struct gpu_display_input_frame *in=&inputs->frame[frame];
        const uint8_t *data=(const uint8_t *)inputs+in->offset;
        struct pvr_render_texture_request q={.operation=PVR_TEXTURE_BEGIN,
            .previous=frame,.input=in->input};uint64_t ticket;
        r=scoped(epoch,&q,NULL,1000000,&ticket);
        if(!r.texture_generation)retained();q.generation=r.texture_generation;
        for(uint32_t offset=0;offset<in->bytes;){
            uint32_t n=in->bytes-offset;if(n>PVR_RENDER_TEXTURE_CHUNK)n=PVR_RENDER_TEXTURE_CHUNK;
            q.operation=PVR_TEXTURE_WRITE;q.offset=offset;q.bytes=n;
            r=scoped(epoch,&q,data+offset,1000000,&ticket);offset+=n;
        }
        q.operation=PVR_TEXTURE_SUBMIT;q.offset=q.bytes=0;
        r=scoped(epoch,&q,NULL,5000000,&ticket);
        if(r.render_sequence!=frame+1)retained();
        while(r.render_phase!=PVR_RENDER_SERVICE_COMPLETE){vTaskDelay(1);r=lifecycle(PVR_GPU_SERVICE_STATUS,0);}
        if(!gpu_display_evidence_begin(frame,in->digest))retained();
        for(uint32_t offset=0;offset<GPU_DISPLAY_WIDTH*GPU_DISPLAY_HEIGHT*4u;){
            uint32_t n=GPU_DISPLAY_WIDTH*GPU_DISPLAY_HEIGHT*4u-offset;if(n>PVR_RENDER_TEXTURE_CHUNK)n=PVR_RENDER_TEXTURE_CHUNK;
            q=(struct pvr_render_texture_request){.operation=PVR_TEXTURE_READ,.previous=frame+1,.offset=offset,.bytes=n};
            r=scoped(epoch,&q,NULL,1000000,&ticket);
            const void *copy;uint32_t bytes;
            if(r.render_sequence!=frame+1 || !pvr_gpu_service_texture_output(&app.gpu_service,ticket,&copy,&bytes) ||
               bytes!=n || !gpu_display_bank_copy(frame,frame+1,offset,copy,bytes))retained();
            /* The display copy completes before returning this exact borrowed
             * snapshot to its original service/client/frame/offset owner. */
            q.operation=PVR_TEXTURE_RELEASE;q.generation=ticket;q.bytes=0;
            r=scoped(epoch,&q,NULL,1000000,&ticket);offset+=n;
        }
        if(!gpu_display_evidence_end(frame))retained();
    }
    r=lifecycle(PVR_GPU_SERVICE_CLOSE,1000000);
    while(r.phase!=PVR_GPU_SERVICE_STOPPED){vTaskDelay(1);r=lifecycle(PVR_GPU_SERVICE_STATUS,0);}
    atomic_store_explicit(&app.gpu_closed,1,memory_order_release);
    if(!gpu_display_bank_seal())retained();
    atomic_store_explicit(&app.frames_ready,1,memory_order_release);
    for(;;)vTaskSuspend(NULL);
}
void gpu_display_irq(void)
{
    BaseType_t woken=pdFALSE;
    if(!pvr_gpu_service_interrupt(&app.gpu_service,&woken))hdmi_fail(NULL);
    portYIELD_FROM_ISR(woken);
}
