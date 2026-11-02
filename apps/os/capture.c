/* One manifest-format capture per sequence. The I2S stream writes directly
 * into the buffer after the application's 64-byte container headroom. */
#include "os.h"
#include "application.h"
#include "capture.h"
#include "boot.h"
#include <string.h>
extern uint8_t ss_capture_buffer[];
/* The headset library's capture: absent (null) in an image that does not link it. */
extern int os_headset_capture(unsigned,const uint8_t **,size_t *) __attribute__((weak));
extern const size_t ss_capture_capacity;
/* Recorded input uses the same copied output and caller headroom as device
 * capture. Only the application task accesses this state. Input authentication
 * belongs to application startup; replay preserves those input bytes. */
static struct {
    const uint8_t *input;
    uint64_t frames,cursor,start,last;
    uint32_t window,frame_bytes,rate,sequence,pending,ready_frames;
    unsigned opened,paced,started,failed;
} replay;
int ss_capture_replay_open(uint32_t region,uint64_t offset,uint32_t frames,uint32_t paced)
{
    const struct ss_grant *g=ss_grant();
    const struct ss_manifest *m=ss_application.manifest;
    if(!g||!m||ss_live()!=1||replay.opened||paced>1||
       (g->devices&(SS_DEVICE_AUDIO_I2S|SS_DEVICE_AUDIO_HEADSET))||
       region>=g->region_count||region>=m->region_count||
       m->regions[region].kind!=SS_REGION_INPUT||!(m->regions[region].access&SS_ACCESS_READ)||
       !m->audio.rate_hz||!m->audio.channels||m->audio.channels>2||
       (m->audio.bits!=16&&m->audio.bits!=24&&m->audio.bits!=32)||
       !frames||frames<m->audio.min_frames||frames>m->audio.max_frames)return 0;
    const struct ss_span *s=&g->regions[region];
    uint32_t frame=m->audio.channels*(m->audio.bits/8);
    uint64_t bytes=(uint64_t)frames*frame;
    uintptr_t address=(uintptr_t)s->base,output=(uintptr_t)ss_capture_buffer;
    if(!s->base||s->bytes>UINTPTR_MAX-address||offset>s->bytes||
       (s->bytes-offset)%frame||bytes>s->bytes-offset||bytes>ss_capture_capacity||
       ss_capture_capacity>UINTPTR_MAX-output-SS_CAPTURE_HEADROOM)return 0;
    /* A caller may overwrite output headroom; input must remain disjoint. */
    if(address<output+SS_CAPTURE_HEADROOM+ss_capture_capacity&&output<address+s->bytes)return 0;
    uint64_t source_frames=(s->bytes-offset)/frame;
    if(source_frames>UINT64_MAX/A733_SMP_COUNTER_HZ)return 0;
    replay.input=(const uint8_t *)s->base+(size_t)offset;
    replay.frames=source_frames;replay.window=frames;replay.frame_bytes=frame;
    replay.rate=m->audio.rate_hz;replay.paced=paced;replay.opened=1;
    return 1;
}
static int replay_read(uint32_t sequence,void **pcm,size_t *bytes)
{
    if(replay.failed||(replay.pending&&sequence<replay.pending))return -1;
    if(sequence!=replay.sequence){
        if(replay.sequence==UINT32_MAX||sequence!=replay.sequence+1||
           (replay.pending&&replay.pending!=sequence))return -1;
        uint64_t remaining=replay.frames-replay.cursor;
        uint32_t frames=remaining<replay.window?(uint32_t)remaining:replay.window;
        if(!frames||frames<ss_application.manifest->audio.min_frames)return -1;
        replay.pending=sequence;
        if(replay.paced){
            uint64_t now=a733_smp_counter();
            if(!replay.started){replay.start=replay.last=now;replay.started=1;}
            if(now<replay.last){replay.failed=1;return -1;}
            replay.last=now;
            uint64_t ticks=(replay.cursor+frames)*A733_SMP_COUNTER_HZ;
            uint64_t due=ticks/replay.rate+(ticks%replay.rate!=0);
            if(now-replay.start<due)return 0;
        }
        size_t n=(size_t)frames*replay.frame_bytes;
        memcpy(ss_capture_buffer+SS_CAPTURE_HEADROOM,replay.input+(size_t)replay.cursor*replay.frame_bytes,n);
        if(ss_live()!=1){replay.failed=1;return -1;}
        replay.cursor+=frames;replay.sequence=sequence;replay.ready_frames=frames;replay.pending=0;
    }
    *pcm=ss_capture_buffer+SS_CAPTURE_HEADROOM;*bytes=(size_t)replay.ready_frames*replay.frame_bytes;return 1;
}
int os_audio_client_admit(uint32_t frames)
{
    if(!ss_application.manifest)return 0;
    const struct ss_audio_format *f=&ss_application.manifest->audio;
    return frames>=f->min_frames&&frames<=f->max_frames;
}
static struct {unsigned stage,sequence,requested;uint32_t bytes,periods,total;struct os_audio_stream_config config;} capture;
int os_capture_start(void)
{
    /* Admission granted the I2S service with the I2S device and its record; without them no pin, clock, DMA channel or
     * interrupt is touched. */
    if(!os_granted(OS_GRANT_AUDIO))return 1;
    if(!ss_application.manifest)return 0;
    const struct os_session *s=os_session_admitted();const struct ss_audio_format *f=&ss_application.manifest->audio;
    if(!s||capture.total||!os_audio_client_admit(s->audio.capture_samples)||!f->rate_hz||
       (f->bits!=16&&f->bits!=32)||!f->channels||f->channels>2)return 0;
    uint64_t total=(uint64_t)s->audio.capture_samples*f->channels*(f->bits/8);
    uint32_t periods=(total+OS_AUDIO_PERIOD_CAPACITY-1)/OS_AUDIO_PERIOD_CAPACITY;
    if(periods<2)periods=2;
    if(periods>OS_AUDIO_RING_CAPACITY||(size_t)periods*OS_AUDIO_PERIOD_CAPACITY>ss_capture_capacity)return 0;
    capture.total=total;
    capture.config=(struct os_audio_stream_config){{f->rate_hz,f->channels,f->bits,{0}},
        OS_AUDIO_PERIOD_CAPACITY,periods,ss_capture_buffer+SS_CAPTURE_HEADROOM,0};
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
int ss_capture(uint32_t sequence,void **pcm,size_t *bytes)
{
    const struct ss_grant *g=ss_grant();
    if(!g||!sequence||!pcm||!bytes||ss_live()!=1)return -1;
    if(replay.opened)return replay_read(sequence,pcm,bytes);
    if(g->devices&SS_DEVICE_AUDIO_I2S){
        if(capture.stage==4||!capture.total||sequence<capture.requested)return -1;
        capture.requested=sequence;if(capture_step()<0){capture.stage=4;return -1;}
        if(capture.stage!=3||capture.sequence!=sequence)return 0;
        *pcm=ss_capture_buffer+SS_CAPTURE_HEADROOM;*bytes=capture.total;return 1;
    }
    if(g->devices&SS_DEVICE_AUDIO_HEADSET){
        const uint8_t *wav;size_t n;int r=os_headset_capture?os_headset_capture(sequence,&wav,&n):-1;
        if(r!=1)return r;if(n<44)return -1;
        const struct ss_audio_format *f=&ss_application.manifest->audio;
        if(f->rate_hz!=16000||f->channels!=1||f->bits!=16||!(n-44)||n%2||n-44>(size_t)g->capture_frames*2)return -1;
        *pcm=(void *)(wav+44);*bytes=n-44;return 1;
    }
    return -1;
}
