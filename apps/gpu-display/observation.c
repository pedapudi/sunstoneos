#include "application.h"
#include "a733_storage_digest.h"
#include "boot.h"
#include <string.h>
static struct {
    struct storage_digest rgba,rgb565;
    uint32_t bytes;
    unsigned begun,ended;
} digest[2];
static int renderer(void)
{return xTaskGetCurrentTaskHandle()==app.handles[2] &&
    !atomic_load_explicit(&app.frames_ready,memory_order_acquire) && hdmi_live(NULL);}
int gpu_display_evidence_begin(unsigned frame,const uint8_t input[32])
{
    if(frame>1||!input||!renderer()||digest[frame].begun)return 0;
    digest[frame].begun=1;storage_digest_init(&digest[frame].rgba);storage_digest_init(&digest[frame].rgb565);
    app.evidence[frame].sequence=frame+1;memcpy(app.evidence[frame].input_sha256,input,32);return 1;
}
int gpu_display_evidence_chunk(unsigned frame,uint32_t offset,const void *rgba,const void *rgb565,uint32_t bytes)
{
    if(frame>1||!renderer()||!digest[frame].begun||digest[frame].ended||!rgba||!rgb565||
       !bytes||((offset|bytes)&3)||offset!=digest[frame].bytes||
       offset>GPU_DISPLAY_WIDTH*GPU_DISPLAY_HEIGHT*4u||bytes>GPU_DISPLAY_WIDTH*GPU_DISPLAY_HEIGHT*4u-offset)return 0;
    if(!storage_digest_add(&digest[frame].rgba,rgba,bytes)||
       !storage_digest_add(&digest[frame].rgb565,rgb565,bytes/2))return 0;
    static const uint32_t x[4]={0,426,853,1279},y[4]={0,239,479,719};
    for(unsigned row=0;row<4;++row)for(unsigned col=0;col<4;++col){
        uint32_t at=(y[row]*GPU_DISPLAY_WIDTH+x[col])*4;
        if(at>=offset&&at-offset<bytes){
            struct gpu_display_sample *sample=&app.evidence[frame].samples[row*4+col];
            memcpy(&sample->rgba,(const uint8_t *)rgba+at-offset,4);
            memcpy(&sample->rgb565,(const uint8_t *)rgb565+(at-offset)/2,2);
        }
    }
    digest[frame].bytes+=bytes;return hdmi_live(NULL);
}
int gpu_display_evidence_end(unsigned frame)
{
    if(frame>1||!renderer()||!digest[frame].begun||digest[frame].ended||
       digest[frame].bytes!=GPU_DISPLAY_WIDTH*GPU_DISPLAY_HEIGHT*4u)return 0;
    storage_digest_end(&digest[frame].rgba,app.evidence[frame].rgba_sha256);
    storage_digest_end(&digest[frame].rgb565,app.evidence[frame].rgb565_sha256);
    app.evidence[frame].rgba_bytes=digest[frame].bytes;app.evidence[frame].flags=1;
    digest[frame].ended=1;return hdmi_live(NULL);
}
int gpu_display_observation_request(const void *data,size_t bytes,uint64_t deadline)
{
    uint32_t words[4];uint64_t identity[2],now;
    if(!data||bytes!=32||xTaskGetCurrentTaskHandle()!=app.handles[3]||
       atomic_load_explicit(&app.observation.state,memory_order_acquire)||
       !hdmi_live(NULL)||deadline>app.deadline||!a733_counter_clock_now(&app.counter,&now)||now>=deadline)return 0;
    memcpy(words,data,16);memcpy(identity,(const uint8_t *)data+16,16);
    if(words[0]!=GPU_DISPLAY_OBSERVE_MAGIC||words[1]!=1||words[3]!=32||
       identity[0]!=app.session.epoch||identity[1]!=app.session.generation||
       !words[2]||app.observation.requests>=GPU_DISPLAY_OBSERVE_LIMIT||
       words[2]!=app.observation.request_id+1)return 0;
    app.observation.request_id=words[2];app.observation.request_deadline=deadline;++app.observation.requests;
    atomic_store_explicit(&app.observation.state,1,memory_order_release);return 1;
}
void gpu_display_observation_poll(void)
{
    if(xTaskGetCurrentTaskHandle()!=app.handles[1]||
       atomic_load_explicit(&app.observation.state,memory_order_acquire)!=1)return;
    uint64_t now;if(!a733_counter_clock_now(&app.counter,&now)||now>=app.deadline||
       now>=app.observation.request_deadline){hdmi_fail(NULL);return;}
    const uint8_t *session_digest=gpu_display_session_digest_get();
    if(!session_digest){hdmi_fail(NULL);return;}
    struct gpu_display_observation *r=&app.observation.reply;memset(r,0,sizeof(*r));
    r->magic=GPU_DISPLAY_OBSERVE_MAGIC;r->version=1;r->bytes=sizeof(*r);r->request_id=app.observation.request_id;
    r->epoch=app.session.epoch;r->generation=app.session.generation;
    r->state=atomic_load_explicit(&app.state,memory_order_acquire);r->now_us=now;r->remaining_us=app.deadline-now;
    if(atomic_load_explicit(&app.frames_ready,memory_order_acquire)){
        /* This release publication follows GPU CLOSE and bank sealing. Neither
         * owner writes these records again. USB never reads renderer state. */
        r->frames=2;r->flags|=1;memcpy(r->frame,app.evidence,sizeof(r->frame));
    }
    r->presentations=app.presentations;memcpy(r->presentation,app.presentation,sizeof(r->presentation));
    r->terminal.gpu_closed=atomic_load_explicit(&app.gpu_closed,memory_order_acquire);
    r->terminal.hdmi_state=app.hdmi.controller.state;r->terminal.de_state=framebuffer.de.state;
    r->terminal.pmic_parked=atomic_load_explicit(&app.pmic_parked,memory_order_acquire);
    r->terminal.fatal=atomic_load_explicit(&app.fatal,memory_order_acquire);
    r->terminal.usb_ready=atomic_load_explicit(&app.usb_ready,memory_order_acquire);
    r->terminal.display_stage=app.stage;r->terminal.stop_at=app.stop_at;r->terminal.deadline=app.deadline;
    r->terminal.lease=app.session.lease;memcpy(r->terminal.build_id,app.session.image,32);
    memcpy(r->terminal.session_sha256,session_digest,32);
    if(r->terminal.gpu_closed)r->flags|=2;
    if(r->state==2)r->flags|=4;
    if(r->terminal.fatal)r->flags|=8;
    /* Only this owner writes the snapshot. TX owns every copied byte until
     * TinyUSB's final completion callback, including its automatic ZLP. */
    atomic_store_explicit(&app.observation.state,2,memory_order_release);
}
