#include "application.h"
#include "observation.h"
#include "usb.h"
#include "native.h"
#include <string.h>
#if SUNSTONE_HDMI_AUDIO_STREAM
#include "stream.h"
#endif
struct hdmi_audio_observation_mailbox observation;
int hdmi_audio_observation_request(const void *data,size_t bytes,uint64_t deadline)
{
#if SUNSTONE_HDMI_AUDIO_STREAM
    uint64_t now;
    struct hdmi_stream_request r;
    if(!data||bytes!=sizeof(r)||xTaskGetCurrentTaskHandle()!=app.handles[2]||
       atomic_load_explicit(&observation.state,memory_order_acquire)||!hdmi_live(NULL)||
       deadline>app.deadline||!a733_counter_clock_now(&app.counter,&now)||now>=deadline)return 0;
    memcpy(&r,data,sizeof(r));
    uint64_t head=atomic_load_explicit(&stream.head,memory_order_acquire);
    uint64_t periods=(app.session.audio.frames+HDMI_STREAM_PERIOD_FRAMES-1)/HDMI_STREAM_PERIOD_FRAMES;
    if(r.magic!=HDMI_AUDIO_REQUEST_MAGIC||r.version!=2||r.bytes!=sizeof(r)||
       r.epoch!=app.session.epoch||r.generation!=app.session.generation||
       !r.request_id||r.request_id!=observation.request_id+1||observation.requests>=2*periods+64||
       r.operation>HDMI_STREAM_CANCEL||r.sequence!=head)return 0;
    if(r.operation==HDMI_STREAM_WRITE){
        unsigned expected=hdmi_stream_upload_bytes(r.sequence);
        if(!expected||r.payload_bytes!=expected||atomic_load_explicit(&stream.closed,memory_order_acquire))return 0;
    }else if(r.payload_bytes)return 0;
    if(r.operation==HDMI_STREAM_END&&!hdmi_stream_end(r.sequence))return 0;
    if(r.operation==HDMI_STREAM_CANCEL&&!hdmi_stream_cancel())return 0;
    observation.request_id=r.request_id;observation.request_deadline=deadline;++observation.requests;
    observation.sequence=r.sequence;observation.operation=r.operation;observation.payload_bytes=r.payload_bytes;
    /* State 4 owns a partially received unpublished input slot. The output
     * task must not publish a response before that complete payload arrives. */
    atomic_store_explicit(&observation.state,r.operation==HDMI_STREAM_WRITE?4:1,memory_order_release);return 1;
#else
    uint32_t words[4];uint64_t identity[2],now;
    if(!data||bytes!=32||xTaskGetCurrentTaskHandle()!=app.handles[2]||
       atomic_load_explicit(&observation.state,memory_order_acquire)||!hdmi_live(NULL)||
       deadline>app.deadline||!a733_counter_clock_now(&app.counter,&now)||now>=deadline)return 0;
    memcpy(words,data,16);memcpy(identity,(const uint8_t *)data+16,16);
    if(words[0]!=HDMI_AUDIO_REQUEST_MAGIC||words[1]!=1||words[3]!=32||
       identity[0]!=app.session.epoch||identity[1]!=app.session.generation||
       !words[2]||observation.requests>=HDMI_AUDIO_OBSERVE_LIMIT||words[2]!=observation.request_id+1)return 0;
    observation.request_id=words[2];observation.request_deadline=deadline;++observation.requests;
    atomic_store_explicit(&observation.state,1,memory_order_release);return 1;
#endif
}
#if SUNSTONE_HDMI_AUDIO_STREAM
int hdmi_audio_observation_uploaded(void)
{
    uint64_t now;
    if(xTaskGetCurrentTaskHandle()!=app.handles[2]||
       atomic_load_explicit(&observation.state,memory_order_acquire)!=4||!hdmi_live(NULL)||
       !a733_counter_clock_now(&app.counter,&now)||now>=observation.request_deadline||
       !hdmi_stream_uploaded(observation.sequence,observation.payload_bytes))return 0;
    atomic_store_explicit(&observation.state,1,memory_order_release);return 1;
}
#endif
void hdmi_audio_observation_poll(void)
{
    if(xTaskGetCurrentTaskHandle()!=app.handles[1]||
       atomic_load_explicit(&observation.state,memory_order_acquire)!=1)return;
    uint64_t now;if(!hdmi_live(NULL)||!a733_counter_clock_now(&app.counter,&now)||
       now>=app.deadline||now>=observation.request_deadline){hdmi_fail(NULL);return;}
    struct hdmi_audio_observation *r=&observation.reply;memset(r,0,sizeof(*r));
    r->magic=HDMI_AUDIO_OBSERVE_MAGIC;r->version=SUNSTONE_HDMI_AUDIO_STREAM?2:1;r->bytes=sizeof(*r);r->request_id=observation.request_id;
    r->epoch=app.session.epoch;r->generation=app.session.generation;r->lease=app.session.lease;
    r->now_us=now;r->remaining_us=app.deadline-now;
    memcpy(r->build_id,app.session.image,32);memcpy(r->session_sha256,usb.session_sha256,32);
    unsigned state=atomic_load_explicit(&app.state,memory_order_acquire);
    if(atomic_load_explicit(&usb.ready,memory_order_acquire))r->flags|=2;
    if(state==HDMI_APP_STOPPED_RETAINED)r->flags|=4;
    if(atomic_load_explicit(&app.fatal,memory_order_acquire))r->flags|=8;
    /* Only the result writer inspects published. USB never races ready's
     * coherence-publication failure path or reads the mutable raw result. */
    if(audio.published){
        if(atomic_load_explicit(&hdmi_audio_result_ready,memory_order_acquire)!=1){hdmi_fail(NULL);return;}
        r->result=hdmi_audio_result;r->flags|=1;
    }
    if(state==HDMI_APP_FAULT_RETAINED||(r->flags&8)||
       ((r->flags&1)&&r->result.stage==HDMI_AUDIO_FAULT_RETAINED))r->state=2;
    else if((r->flags&7)==7&&r->result.stage==HDMI_AUDIO_INPUT_ENDED_RETAINED)r->state=1;
#if SUNSTONE_HDMI_AUDIO_STREAM
    if((r->flags&7)==7&&r->result.stage==HDMI_AUDIO_CANCELED_RETAINED)r->state=3;
    uint64_t accepted=atomic_load_explicit(&stream.head,memory_order_acquire)*HDMI_STREAM_PERIOD_FRAMES;
    if(accepted>app.session.audio.frames)accepted=app.session.audio.frames;
    struct hdmi_stream_observation counters={.credits=hdmi_stream_credits(),.command_result=0,
        .accepted_frames=accepted,.committed_frames=atomic_load_explicit(&stream.committed,memory_order_acquire),
        .consumed_frames=atomic_load_explicit(&stream.consumed,memory_order_acquire),.sequence=observation.sequence};
    memcpy(r->reserved,&counters,sizeof(counters));
#endif
    atomic_store_explicit(&observation.state,2,memory_order_release);
}
