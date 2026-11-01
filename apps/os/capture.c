/* One manifest-format capture per sequence. The I2S stream writes directly
 * into the buffer after the application's 64-byte container headroom. */
#include "os.h"
#include "application.h"
#include <string.h>
int os_audio_client_admit(uint32_t frames)
{
    const struct ss_audio_format *f=&ss_application.manifest->audio;
    return frames>=f->min_frames&&frames<=f->max_frames;
}
#ifdef SUNSTONE_A733_OS_AUDIO
extern uint8_t ss_capture_buffer[];
extern const size_t ss_capture_capacity;
static struct {unsigned stage,sequence,requested;uint32_t bytes,periods,total;struct os_audio_stream_config config;} capture;
int os_capture_start(void)
{
    const struct os_session *s=os_session_admitted();const struct ss_audio_format *f=&ss_application.manifest->audio;
    if(!s||capture.total||!os_audio_client_admit(s->audio.capture_samples)||!f->rate_hz||
       (f->bits!=16&&f->bits!=32)||!f->channels||f->channels>2)return 0;
    uint64_t total=(uint64_t)s->audio.capture_samples*f->channels*(f->bits/8);
    uint32_t periods=(total+OS_AUDIO_PERIOD_CAPACITY-1)/OS_AUDIO_PERIOD_CAPACITY;
    if(periods<2)periods=2;
    if(periods>OS_AUDIO_RING_CAPACITY||(size_t)periods*OS_AUDIO_PERIOD_CAPACITY>ss_capture_capacity)return 0;
    capture.total=total;
    capture.config=(struct os_audio_stream_config){{f->rate_hz,f->channels,f->bits,{0}},
        OS_AUDIO_PERIOD_CAPACITY,periods,ss_capture_buffer+SS_CAPTURE_HEADROOM};
    return os_audio_stream_open(&capture.config);
}
static int capture_step(void)
{
    int r;
    if(capture.stage==0||capture.stage==3){
        if(capture.requested==capture.sequence)return os_audio_stream_stop()<0?-1:0;
        r=os_audio_stream_start();if(r<=0)return r;
        capture.sequence=capture.requested;capture.bytes=capture.periods=0;capture.stage=1;
    }
    if(capture.stage==1){
        struct os_audio_period p;
        while((r=os_audio_stream_read(&p))==1){
            uint32_t frame_bytes=capture.config.format.channels*(capture.config.format.bits/8);
            if(p.data!=ss_capture_buffer+SS_CAPTURE_HEADROOM+(size_t)capture.periods*OS_AUDIO_PERIOD_CAPACITY||
               p.bytes!=OS_AUDIO_PERIOD_CAPACITY||p.frames!=p.bytes/frame_bytes||
               p.first_frame!=(uint64_t)capture.periods*(OS_AUDIO_PERIOD_CAPACITY/frame_bytes))return -1;
            uint32_t n=capture.total-capture.bytes;if(n>p.bytes)n=p.bytes;
            capture.bytes+=n;++capture.periods;
            if(capture.bytes==capture.total){capture.stage=2;break;}
        }
        if(r<0)return -1;
    }
    if(capture.stage==2){r=os_audio_stream_stop();if(r<0)return -1;if(r==1)capture.stage=3;}
    return 0;
}
#else
int os_capture_start(void){return 0;}
#endif
int ss_capture(uint32_t sequence,void **pcm,size_t *bytes)
{
    const struct ss_grant *g=ss_grant();
    if(!g||!sequence||!pcm||!bytes||ss_live()!=1)return -1;
#ifdef SUNSTONE_A733_OS_AUDIO
    if(g->devices&SS_DEVICE_AUDIO_I2S){
        if(capture.stage==4||!capture.total||sequence<capture.requested)return -1;
        capture.requested=sequence;if(capture_step()<0){capture.stage=4;return -1;}
        if(capture.stage!=3||capture.sequence!=sequence)return 0;
        *pcm=ss_capture_buffer+SS_CAPTURE_HEADROOM;*bytes=capture.total;return 1;
    }
#endif
#ifdef SUNSTONE_A733_OS_HEADSET
    if(g->devices&SS_DEVICE_AUDIO_HEADSET){
        const uint8_t *wav;size_t n;int r=os_headset_capture(sequence,&wav,&n);
        if(r!=1)return r;if(n<44)return -1;
        const struct ss_audio_format *f=&ss_application.manifest->audio;
        if(f->rate_hz!=16000||f->channels!=1||f->bits!=16||!(n-44)||n%2||n-44>(size_t)g->capture_frames*2)return -1;
        *pcm=(void *)(wav+44);*bytes=n-44;return 1;
    }
#endif
    return -1;
}
