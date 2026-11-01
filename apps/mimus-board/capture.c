/* The recognizer's WAV adapter on the OS capture stream (capture.h). Each
 * generation starts a run, reads its periods in place until the capture
 * length is complete, stops the run and writes the WAV header once the
 * stream reports the DMA drained. A period out of place or out of order, or
 * any stream failure, fails every later request. */
#include "capture.h"
#include <string.h>

_Static_assert(MIMUS_CAPTURE_PERIOD_BYTES<=OS_AUDIO_PERIOD_CAPACITY&&
    MIMUS_CAPTURE_PERIODS<=OS_AUDIO_RING_CAPACITY,"the WAV fits one stream run");
enum { WAV_HEADER=44, PERIOD_FRAMES=MIMUS_CAPTURE_PERIOD_BYTES/2 };
enum { IDLE, CAPTURE, STOP, CAPTURED, FAILED };
static struct {
    struct os_audio_stream_config config;
    unsigned stage,generation,requested;
    uint32_t samples,bytes,periods;
    _Alignas(4) uint8_t wav[WAV_HEADER+MIMUS_CAPTURE_PERIODS*MIMUS_CAPTURE_PERIOD_BYTES];
} capture;

static void put16(uint8_t *p,unsigned v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}
static void put32(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(v>>(8*i));}
const struct os_audio_stream_config *mimus_capture_configure(uint32_t samples)
{
    if(!mimus_capture_admit(samples)||capture.samples)return NULL;
    capture.samples=samples;
    capture.config=(struct os_audio_stream_config){{MIMUS_CAPTURE_RATE_HZ,1,16,{0}},
        MIMUS_CAPTURE_PERIOD_BYTES,MIMUS_CAPTURE_PERIODS,capture.wav+WAV_HEADER};
    return &capture.config;
}
/* One bounded step: 0 continue, -1 failed. */
static int step(void)
{
    int r;
    switch(capture.stage){
    case IDLE:
    case CAPTURED:
        /* The stream reports a failure even between runs. */
        if(capture.requested==capture.generation)return os_audio_stream_stop()<0?-1:0;
        r=os_audio_stream_start();if(r<0)return -1;if(!r)return 0;
        capture.generation=capture.requested;capture.bytes=capture.periods=0;capture.stage=CAPTURE;
        /* fall through */
    case CAPTURE:{
        uint32_t total=2*capture.samples;
        struct os_audio_period p;
        while((r=os_audio_stream_read(&p))==1){
            if(p.data!=capture.wav+WAV_HEADER+capture.bytes||p.bytes!=MIMUS_CAPTURE_PERIOD_BYTES||
               p.frames!=PERIOD_FRAMES||p.first_frame!=(uint64_t)capture.periods*PERIOD_FRAMES)return -1;
            uint32_t n=total-capture.bytes;if(n>p.bytes)n=p.bytes;
            capture.bytes+=n;++capture.periods;
            if(capture.bytes==total){capture.stage=STOP;break;}
        }
        if(r<0)return -1;
        if(capture.stage!=STOP)return 0;}
        /* fall through */
    case STOP:{
        r=os_audio_stream_stop();if(r<0)return -1;if(!r)return 0;
        uint8_t *w=capture.wav;memcpy(w,"RIFF",4);put32(w+4,36+capture.bytes);memcpy(w+8,"WAVEfmt ",8);
        put32(w+16,16);put16(w+20,1);put16(w+22,1);put32(w+24,MIMUS_CAPTURE_RATE_HZ);
        put32(w+28,2*MIMUS_CAPTURE_RATE_HZ);put16(w+32,2);put16(w+34,16);memcpy(w+36,"data",4);
        put32(w+40,capture.bytes);capture.stage=CAPTURED;return 0;}
    default:return -1;
    }
}
int mimus_capture(unsigned g,const uint8_t **wav,size_t *bytes)
{
    if(capture.stage==FAILED||!capture.samples||!g||!wav||!bytes||g<capture.requested)return -1;
    capture.requested=g;
    if(step()<0){capture.stage=FAILED;return -1;}
    if(capture.stage!=CAPTURED||capture.generation!=g)return 0;
    *wav=capture.wav;*bytes=WAV_HEADER+2*capture.samples;return 1;
}
