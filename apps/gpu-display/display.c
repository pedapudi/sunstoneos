/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "application.h"
#include "a733_display_native.h"
#include "a733_memory_uncached.h"
#include <string.h>
#define IOVA UINT64_C(0x10000000)
#define FRAME_OFFSET GPU_DISPLAY_FRAME_OFFSET
struct a733_display_framebuffer framebuffer;
static struct hdmi_framebuffer_session sealed;
static int current(void *v)
{return v==&app&&hdmi_live(NULL)&&xTaskGetCurrentTaskHandle()==app.handles[1]&&
    !memcmp(&sealed,&app.session.framebuffer,sizeof(sealed));}
static int output_owned(void *v)
{
    return v==&app&&a733_clock_token_status(&app.clocks,&app.hdmi.clock)==A733_CLOCK_OK&&
        a733_rail_status(&app.rails,&app.hdmi.rail)==A733_RAIL_READY&&
        a733_pmic_live(&app.pmic,&app.hdmi.pmic_lifetime)==A733_PMIC_READY&&
        a733_gpu_resources_status(&app.pck,app.session.pck_client,&app.hdmi.pck_lease)==A733_GPU_RESOURCE_OK;
}
static int admit(void *v,enum a733_de_action action)
{
    enum a733_hdmi_state h=app.hdmi.controller.state;
    if(v!=&app)return 0;
    if(action==A733_DE_PREPARE||action==A733_DE_START)return h==A733_HDMI_PREPARED;
    if(action==A733_DE_REPLAY)return h==A733_HDMI_ACTIVE &&
        atomic_load_explicit(&app.frames_ready,memory_order_acquire) &&
        a733_de_replay_valid((void *)(framebuffer.extent.cpu+FRAME_OFFSET+2u*GPU_DISPLAY_FRAME_BYTES),
            IOVA+4096,GPU_DISPLAY_FRAME_BYTES);
    if(action==A733_DE_STOP)return h==A733_HDMI_STOPPED_RETAINED;
    return h==A733_HDMI_ACTIVE;
}
static int replay(void *v,uint64_t deadline)
{return v==&app&&deadline<=app.stop_at&&app.hdmi.controller.state==A733_HDMI_ACTIVE;}
static int bank_cpu_owned(void)
{
    struct device_memory_extent e;enum device_memory_state state;
    return framebuffer.self==&framebuffer && !framebuffer.failed && framebuffer.boot_ready &&
        framebuffer.pool==a733_memory_uncached_init() &&
        device_memory_describe(framebuffer.pool,framebuffer.lease,&e,&state) &&
        state==DEVICE_MEMORY_CPU && !memcmp(&e,&framebuffer.extent,sizeof(e));
}
int gpu_display_bank_copy(unsigned frame,uint32_t sequence,uint32_t offset,const void *data,uint32_t bytes)
{
    if(frame>1 || sequence!=frame+1 || !hdmi_live(NULL) ||
       xTaskGetCurrentTaskHandle()!=app.handles[2] || !bank_cpu_owned() ||
       atomic_load_explicit(&app.frames_ready,memory_order_acquire) ||
       offset!=app.copied[frame] || !bytes || ((offset|bytes)&3) ||
       offset>GPU_DISPLAY_WIDTH*GPU_DISPLAY_HEIGHT*4u ||
       bytes>GPU_DISPLAY_WIDTH*GPU_DISPLAY_HEIGHT*4u-offset)return 0;
    struct pvr_render_service *source=&app.gpu_graphics.submission;
    const void *owned;uint32_t owned_bytes;
    if(source->phase!=PVR_RENDER_SERVICE_COMPLETE || source->sequence!=sequence ||
       source->texture_output_frame!=sequence || source->texture_output_offset!=offset ||
       !pvr_gpu_service_texture_output(&app.gpu_service,source->texture_output_ticket,&owned,&owned_bytes) ||
       owned!=data || owned_bytes!=bytes)return 0;
    const uint8_t *in=data;
    uint16_t *out=(void *)(framebuffer.extent.cpu+FRAME_OFFSET+frame*GPU_DISPLAY_FRAME_BYTES+offset/2);
    for(uint32_t i=0;i<bytes;i+=4)
        out[i/4]=(uint16_t)(((uint16_t)(in[i]>>3)<<11)|((uint16_t)(in[i+1]>>2)<<5)|(in[i+2]>>3));
    if(!gpu_display_evidence_chunk(frame,offset,in,out,bytes))return 0;
    app.copied[frame]+=bytes;app.frame_sequence[frame]=sequence;
    return hdmi_live(NULL);
}
int gpu_display_bank_seal(void)
{
    if(!hdmi_live(NULL) || xTaskGetCurrentTaskHandle()!=app.handles[2] || !bank_cpu_owned() ||
       !atomic_load_explicit(&app.gpu_closed,memory_order_acquire) ||
       app.copied[0]!=GPU_DISPLAY_WIDTH*GPU_DISPLAY_HEIGHT*4u ||
       app.copied[1]!=GPU_DISPLAY_WIDTH*GPU_DISPLAY_HEIGHT*4u ||
       app.frame_sequence[0]!=1 || app.frame_sequence[1]!=2)return 0;
    void *commands=(void *)(framebuffer.extent.cpu+FRAME_OFFSET+2u*GPU_DISPLAY_FRAME_BYTES);
    if(!a733_de_replay_build(commands,IOVA+4096,GPU_DISPLAY_FRAME_BYTES))return 0;
    /* NC mappings were verified by the actual pool constructor; no cached alias
     * or CPU cache clean is substituted for publication. No device has begun
     * fetching this bank. Every byte remains immutable after this barrier. */
    if(!a733_display_native_barrier() || !hdmi_live(NULL))return 0;
    return a733_display_framebuffer_publish(&framebuffer);
}
int hdmi_framebuffer_boot(void)
{
    if(!hdmi_framebuffer_session_valid(&app.session.framebuffer))return 0;
    sealed=app.session.framebuffer;
    struct a733_display_framebuffer_config c={.counter=&app.counter,.clocks=&app.clocks,
        .locks=&app.locks,.pck=&app.pck,.pck_lease=&app.hdmi.pck_lease,
        .pck_client=app.session.pck_client,.owner=app.session.owner,.authority=sealed.authority,
        .deadline=app.deadline,.parent_bytes=GPU_DISPLAY_PARENT_BYTES,.frame_span=4096u+GPU_DISPLAY_BANK_BYTES,
        .output=A733_DE_TV0_RGB565,.context=&app,.current=current,.output_owned=output_owned,.admit=admit,
        .replay=replay,.rcq_submit=a733_display_native_rcq_submit,
        .domains=&app.domains,.rail=&app.hdmi.rail};
    memcpy(c.fabric,sealed.fabric,sizeof(c.fabric));memcpy(c.fabric_hz,sealed.fabric_hz,sizeof(c.fabric_hz));
    memcpy(c.iommu_gates,sealed.iommu_gates,sizeof(c.iommu_gates));
    /* The bank stays CPU-owned until both copied frames are sealed. */
    return a733_display_framebuffer_boot(&framebuffer,&c,0);
}
int hdmi_framebuffer_prepare(void)
{return app.hdmi.controller.state==A733_HDMI_PREPARED?a733_display_framebuffer_prepare(&framebuffer):-1;}
int hdmi_framebuffer_stop(void)
{return app.hdmi.controller.state==A733_HDMI_STOPPED_RETAINED?a733_display_framebuffer_stop(&framebuffer):-1;}

static int output_fault(void)
{
    hdmi_fail(NULL);a733_display_framebuffer_retain(&framebuffer);
    (void)a733_hdmi_fail(&app.hdmi.controller.io);
    atomic_store_explicit(&app.state,3,memory_order_release);return -1;
}
int gpu_display_output_step(void)
{
    if(xTaskGetCurrentTaskHandle()!=app.handles[1])return -1;
    unsigned terminal=atomic_load_explicit(&app.state,memory_order_acquire);
    if(terminal==2)return 1;
    if(terminal==3)return output_fault();
    if(!hdmi_live(NULL))return output_fault();
    uint64_t time;
    if(!a733_counter_clock_now(&app.counter,&time)||time<app.last_now||time>=app.deadline)return output_fault();
    app.last_now=time;
    if(atomic_load_explicit(&app.state,memory_order_acquire)==2)return 1;
    struct a733_hdmi *h=&app.hdmi.controller;
    if(time>=app.stop_at && app.stage<4){
        if(h->state==A733_HDMI_ACTIVE||h->state==A733_HDMI_PREPARED||h->state==A733_HDMI_STARTING){
            if(a733_hdmi_stop(h,app.deadline)!=1)return output_fault();app.stage=4;
        }else return output_fault();
    }
    if(!atomic_load_explicit(&app.pmic_ready,memory_order_acquire))return 0;
    int r;
    switch(app.stage){
    case 0:
        if(a733_hdmi_prepare(h,app.deadline)!=1)return output_fault();app.stage=1;return 0;
    case 1:
        r=a733_hdmi_poll(h);if(r<0)return output_fault();if(!r)return 0;
        if(h->state!=A733_HDMI_PREPARED)return output_fault();
        if(!atomic_load_explicit(&app.frames_ready,memory_order_acquire))return 0;
        r=hdmi_framebuffer_prepare();if(r<0)return output_fault();if(!r)return 0;
        if(a733_hdmi_start(h,app.deadline)!=1)return output_fault();app.stage=2;return 0;
    case 2:
        r=a733_hdmi_poll(h);if(r<0)return output_fault();if(!r)return 0;
        if(h->state!=A733_HDMI_ACTIVE)return output_fault();
        atomic_store_explicit(&app.state,1,memory_order_release);app.stage=3;return 0;
    case 3:
        /* HPD and controller underflow stay observed while RCQ is pending.
         * Only the IOMMU scan is mutually exclusive with replay polling. */
        r=a733_hdmi_service(h,app.deadline);if(r<0 || h->state!=A733_HDMI_ACTIVE)return output_fault();
        if(app.presentations<3){
            static const unsigned slots[3]={0,1,0};
            if(framebuffer.de.state==A733_DE_PENDING){
                if(framebuffer.de.action!=A733_DE_REPLAY)return output_fault();
                enum a733_de_state state=a733_de_poll(&framebuffer.de);
                if(state==A733_DE_PENDING)return 0;
                if(state!=A733_DE_ARMED || framebuffer.de.replay_slot!=slots[app.presentations] ||
                   !framebuffer.de.replay_rcq_done || framebuffer.de.replay_frame_end<framebuffer.de.replay_rcq_done)
                    return output_fault();
                uint64_t observed;
                if(!a733_counter_clock_now(&app.counter,&observed)||observed<time||
                   observed<framebuffer.de.replay_frame_end||observed>=app.deadline)return output_fault();
                app.presented[app.presentations]=app.frame_sequence[slots[app.presentations]];
                app.presentation[app.presentations]=(struct gpu_display_presentation){
                    .slot=slots[app.presentations],.sequence=app.presented[app.presentations],
                    .rcq_done_us=framebuffer.de.replay_rcq_done,.frame_end_us=framebuffer.de.replay_frame_end,
                    .verified_source_address=framebuffer.de.config.surface.device_address+
                        slots[app.presentations]*GPU_DISPLAY_FRAME_BYTES,
                    .raw_status=framebuffer.de.raw_status,.flags=1,.observed_us=observed};
                ++app.presentations;return 0;
            }
            /* Never run a fault scan concurrently with RCQ admission/polling.
             * The replay callback requires the completed ACTIVE IOMMU state. */
            r=a733_display_framebuffer_service(&framebuffer);if(r<0)return output_fault();if(!r)return 0;
            if(framebuffer.iommu.state!=A733_IOMMU_ACTIVE ||
               !a733_de_replay_request(&framebuffer.de,slots[app.presentations],GPU_DISPLAY_FRAME_BYTES,app.stop_at))
                return output_fault();
            return 0;
        }
        r=a733_display_framebuffer_service(&framebuffer);if(r<0)return output_fault();return r;
    case 4:
        r=a733_hdmi_poll(h);if(r<0)return output_fault();if(!r)return 0;
        if(h->state!=A733_HDMI_STOPPED_RETAINED)return output_fault();
        r=hdmi_framebuffer_stop();if(r<0)return output_fault();if(!r)return 0;
        if(app.presentations!=3 || app.presented[0]!=1 || app.presented[1]!=2 || app.presented[2]!=1)
            return output_fault();
        /* The service blocks until it is notified or its deadline. */
        if(!atomic_exchange_explicit(&app.pmic_park_request,1,memory_order_acq_rel))xTaskNotifyGive(app.handles[0]);
        if(!atomic_load_explicit(&app.pmic_parked,memory_order_acquire))return 0;
        atomic_store_explicit(&app.state,2,memory_order_release);return 1;
    default:return output_fault();
    }
}
void gpu_display_output_task(void *unused)
{(void)unused;for(;;){(void)gpu_display_output_step();gpu_display_observation_poll();vTaskDelay(1);}}
