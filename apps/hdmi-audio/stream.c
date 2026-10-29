#include "application.h"
#include "stream.h"
#include "native.h"
#include "a733_storage_digest.h"
#include <string.h>
struct hdmi_stream stream;
enum { OPEN, READY, IDLE, GET, FILL, COMMIT, START, STATUS, RETURN, CANCEL, STOP_STATUS };
static uint64_t periods(void)
{return (app.session.audio.frames+HDMI_STREAM_PERIOD_FRAMES-1)/HDMI_STREAM_PERIOD_FRAMES;}
size_t hdmi_stream_manifest_bytes(void)
{return sizeof(struct hdmi_stream_manifest)+(size_t)periods()*32;}
int hdmi_stream_manifest_valid(void)
{
    const struct hdmi_stream_manifest *m=(const void *)HDMI_AUDIO_PCM_ADDRESS;
    uint8_t digest[32];size_t bytes=hdmi_stream_manifest_bytes();
    if(!app.session.audio.frames || app.session.audio.frames>HDMI_AUDIO_MAX_FRAMES || bytes>18080 ||
       m->magic!=HDMI_STREAM_MANIFEST_MAGIC || m->version!=1 || m->bytes!=bytes ||
       m->frames!=app.session.audio.frames || m->frame_bytes!=4 || m->period_frames!=1024 ||
       memcmp(m->pcm_sha256,app.session.audio.pcm_sha256,32))return 0;
    storage_digest_bytes(m,bytes,digest);
    return !memcmp(digest,app.session.manifest_sha256,32);
}
unsigned hdmi_stream_upload_bytes(uint64_t sequence)
{
    if(sequence>=periods())return 0;
    uint64_t left=app.session.audio.frames-sequence*1024;
    return (unsigned)(left>1024?4096:left*4);
}
unsigned hdmi_stream_credits(void)
{
    uint64_t tail=atomic_load_explicit(&stream.tail,memory_order_acquire);
    uint64_t head=atomic_load_explicit(&stream.head,memory_order_acquire);
    if(atomic_load_explicit(&stream.closed,memory_order_acquire) || head<tail || head-tail>4)return 0;
    return (unsigned)(4-(head-tail));
}
static int producer(void)
{return xTaskGetCurrentTaskHandle()==app.handles[2] && hdmi_live(NULL) &&
    !atomic_load_explicit(&stream.closed,memory_order_acquire);}
void *hdmi_stream_upload_buffer(uint64_t sequence,unsigned offset,unsigned bytes)
{
    unsigned expected=hdmi_stream_upload_bytes(sequence);
    if(!producer() || sequence!=atomic_load_explicit(&stream.head,memory_order_acquire) ||
       !hdmi_stream_credits() || !bytes || offset>expected || bytes>expected-offset)return NULL;
    return stream.staging[sequence%4]+offset;
}
int hdmi_stream_uploaded(uint64_t sequence,unsigned bytes)
{
    if(!producer() || sequence!=atomic_load_explicit(&stream.head,memory_order_acquire) ||
       !hdmi_stream_credits() || !bytes || bytes!=hdmi_stream_upload_bytes(sequence))return 0;
    atomic_store_explicit(&stream.head,sequence+1,memory_order_release);return 1;
}
int hdmi_stream_end(uint64_t sequence)
{
    if(!producer() || sequence!=periods() || sequence!=atomic_load_explicit(&stream.head,memory_order_acquire))return 0;
    atomic_store_explicit(&stream.closed,1,memory_order_release);
    atomic_store_explicit(&stream.eof,1,memory_order_release);return 1;
}
int hdmi_stream_cancel(void)
{
    if(xTaskGetCurrentTaskHandle()!=app.handles[2] || !hdmi_live(NULL) ||
       atomic_load_explicit(&stream.cancel,memory_order_acquire))return 0;
    atomic_store_explicit(&stream.closed,1,memory_order_release);
    atomic_store_explicit(&stream.cancel,1,memory_order_release);return 1;
}
static int fault(void)
{
    atomic_store_explicit(&stream.closed,1,memory_order_release);
    atomic_store_explicit(&stream.failed,1,memory_order_release);
    hdmi_audio_retain();return -1;
}
static int response(enum dma_service_operation op,uint64_t timeout,uint64_t ticket)
{return hdmi_audio_stream_request(op,timeout,ticket,&stream.reply);}
static int lease(void)
{
    struct a733_i2s_period *p=&stream.reply.period;
    if(!stream.reply.owns_session || !stream.reply.holds_buffers || !p->ticket || p->slot>=4 ||
       (p->kind!=A733_I2S_TX_FILL && p->kind!=A733_I2S_TX_MEMORY_CONSUMED))return 0;
    if(p->kind==A733_I2S_TX_MEMORY_CONSUMED){
        uint64_t consumed=atomic_load_explicit(&stream.consumed,memory_order_relaxed);
        if(!p->frames || p->frames>1024 || p->first_frame!=consumed ||
           p->frames>app.session.audio.frames-consumed)return 0;
        atomic_store_explicit(&stream.consumed,consumed+p->frames,memory_order_release);
    }
    stream.held=p->ticket;stream.slot=p->slot;return 1;
}
int hdmi_stream_step(void)
{
    if(xTaskGetCurrentTaskHandle()!=app.handles[1])return fault();
    /* Each burst performs at most eight metadata transitions. A missing queue
     * reply or input returns to the existing one-tick output-task wait. */
    for(unsigned burst=0;burst<8;++burst){
        uint64_t t;
        if(!hdmi_live(NULL) || !a733_counter_clock_now(&app.counter,&t) || t>=app.stop_at)return fault();
        if(atomic_load_explicit(&stream.cancel,memory_order_acquire) && stream.stage>=IDLE &&
           stream.stage<CANCEL && !stream.request_ticket){
            /* A collected BUSY is no outstanding request. Preserve any leased
             * period and unpublished USB bytes; cancellation does not PUT them. */
            stream.request_until=0;stream.stage=CANCEL;
        }
        int r;
        switch(stream.stage){
        case OPEN:
            r=response(DMA_SERVICE_OPEN,100000,0);if(r<0)return fault();if(!r)return 0;
            if(!stream.reply.owns_session || !stream.reply.holds_buffers)return fault();
            stream.stage=READY;break;
        case READY:
            r=response(DMA_SERVICE_STATUS,0,0);if(r<0)return fault();if(!r)return 0;
            if(stream.reply.phase==DMA_SERVICE_OPENING)return 0;
            if(stream.reply.phase!=DMA_SERVICE_READY)return fault();stream.stage=IDLE;break;
        case IDLE:
            if(!stream.started && (stream.primed==4 || stream.sealed)){stream.stage=START;break;}
            if(stream.sealed && atomic_load_explicit(&stream.consumed,memory_order_acquire)==app.session.audio.frames){
                stream.stage=STATUS;break;
            }
            if(stream.sealed || atomic_load_explicit(&stream.tail,memory_order_acquire)<
                atomic_load_explicit(&stream.head,memory_order_acquire)){stream.stage=GET;break;}
            if(stream.started){stream.stage=STATUS;break;}return 0;
        case GET:
            r=response(DMA_SERVICE_PERIOD_GET,0,0);if(r<0)return fault();if(!r)return 0;
            if(!lease())return fault();stream.stage=stream.sealed?RETURN:FILL;break;
        case FILL:{
            uint64_t tail=atomic_load_explicit(&stream.tail,memory_order_relaxed);
            unsigned bytes=hdmi_stream_upload_bytes(tail);
            if(!stream.held || !bytes || tail>=atomic_load_explicit(&stream.head,memory_order_acquire))return fault();
            /* The final lease stays client-owned until explicit host END. */
            if(tail+1==periods() && !atomic_load_explicit(&stream.eof,memory_order_acquire))return 0;
            uint8_t digest[32];storage_digest_bytes(stream.staging[tail%4],bytes,digest);
            if(memcmp(digest,(const uint8_t *)HDMI_AUDIO_PCM_ADDRESS+64+tail*32,32)){
                atomic_store_explicit(&stream.closed,1,memory_order_release);
                stream.stop_failed=1;stream.stage=CANCEL;break;
            }
            memcpy((void *)(audio.extent.cpu+4096*(stream.slot+1)),stream.staging[tail%4],bytes);
            stream.final_frames=bytes/4;stream.uploaded=(tail+1==periods());
            /* CPU access ends before the command is enqueued. */
            atomic_store_explicit(&stream.tail,tail+1,memory_order_release);
            stream.stage=COMMIT;break;}
        case COMMIT:
            r=response(stream.uploaded?DMA_SERVICE_PERIOD_END:DMA_SERVICE_PERIOD_PUT,
                stream.uploaded?stream.final_frames:0,stream.held);
            if(r<0)return fault();if(!r)return 0;
            atomic_fetch_add_explicit(&stream.committed,stream.final_frames,memory_order_release);
            stream.held=0;++stream.primed;stream.sealed=stream.uploaded;stream.stage=IDLE;break;
        case START:{
            uint64_t duration=(app.session.audio.frames*1000000+47999)/48000;
            if(t>app.stop_at || duration+100000>app.stop_at-t)return fault();
            r=response(DMA_SERVICE_SUBMIT,2400000,0);if(r<0)return fault();if(!r)return 0;
            if(stream.reply.phase!=DMA_SERVICE_RUNNING || !stream.reply.holds_buffers)return fault();
            if(!atomic_load_explicit(&stream.cancel,memory_order_acquire) &&
               a733_hdmi_audio_mute(&audio.sampler,0)!=1)return fault();
            stream.started=1;hdmi_audio_result.stage=HDMI_AUDIO_RUNNING;stream.stage=IDLE;break;}
        case STATUS:
            r=response(DMA_SERVICE_STATUS,0,0);if(r<0)return fault();if(!r)return 0;
            if(stream.reply.phase==DMA_SERVICE_RUNNING || stream.reply.phase==DMA_SERVICE_DRAINING){stream.stage=IDLE;return 0;}
            if(stream.reply.phase!=DMA_SERVICE_RESULT || stream.reply.outcome!=DMA_SERVICE_COMPLETE ||
               !stream.reply.holds_buffers || !stream.sealed || !atomic_load_explicit(&stream.eof,memory_order_acquire))return fault();
            if(atomic_load_explicit(&stream.consumed,memory_order_acquire)!=app.session.audio.frames){stream.stage=GET;break;}
            if(atomic_load_explicit(&stream.committed,memory_order_acquire)!=app.session.audio.frames)return fault();
            return hdmi_audio_stream_publish(HDMI_AUDIO_INPUT_ENDED_RETAINED);
        case RETURN:
            r=response(DMA_SERVICE_PERIOD_PUT,0,stream.held);if(r<0)return fault();if(!r)return 0;
            stream.held=0;stream.stage=IDLE;break;
        case CANCEL:
            r=response(DMA_SERVICE_CANCEL,0,0);if(r<0)return fault();if(!r)return 0;
            stream.stage=STOP_STATUS;break;
        case STOP_STATUS:
            r=response(DMA_SERVICE_STATUS,0,0);if(r<0)return fault();if(!r)return 0;
            if(stream.reply.phase==DMA_SERVICE_DRAINING)return 0;
            if(stream.reply.phase!=DMA_SERVICE_RESULT || stream.reply.outcome!=DMA_SERVICE_CANCELED ||
               !stream.reply.holds_buffers)return fault();
            if(stream.stop_failed)return fault();
            return hdmi_audio_stream_publish(HDMI_AUDIO_CANCELED_RETAINED);
        default:return fault();
        }
    }
    return 0;
}
