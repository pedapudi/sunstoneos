/* Actual capture implementation; only grants, clock and liveness are modeled.
 * No I2S or headset feature is compiled into this fixture. */
#include "../apps/os/capture.c"
#ifdef AUDIO_REPLAY_HOST
#include <stdlib.h>
#endif
volatile unsigned audio_replay_result;
static struct ss_manifest manifest;
const struct ss_application ss_application={.manifest=&manifest};
static struct ss_grant grant;
static uint64_t counter;
static int active=1,granted=1;
static uint8_t input[8192];
uint8_t ss_capture_buffer[SS_CAPTURE_HEADROOM+8192];
const size_t ss_capture_capacity=8192;
const struct ss_grant *ss_grant(void){return granted?&grant:NULL;}
int ss_live(void){return active?1:-1;}
uint64_t a733_smp_counter(void){return counter;}
static void check(int ok,unsigned code)
{
    if(ok)return;
    audio_replay_result=code;
#ifdef AUDIO_REPLAY_HOST
    abort();
#else
    __asm__ volatile("brk #0");
#endif
}
static void reset(unsigned frames,unsigned channels,unsigned bits,unsigned header)
{
    memset(&replay,0,sizeof(replay));memset(&grant,0,sizeof(grant));memset(&manifest,0,sizeof(manifest));
    for(size_t i=0;i<sizeof(input);++i)input[i]=(uint8_t)(i*17+7);
    memset(ss_capture_buffer,0xa5,sizeof(ss_capture_buffer));
    manifest.audio=(struct ss_audio_format){16000,channels,bits,1,1024};
    manifest.region_count=grant.region_count=1;
    manifest.regions[0]=(struct ss_region){.kind=SS_REGION_INPUT,.access=SS_ACCESS_READ,.bytes=sizeof(input)};
    grant.regions[0]=(struct ss_span){input,header+(size_t)frames*channels*(bits/8)};
    counter=0;active=granted=1;
}
static void payload(uint32_t sequence,unsigned first,unsigned frames,unsigned header)
{
    void *pcm=NULL;size_t bytes=0;
    check(ss_capture(sequence,&pcm,&bytes)==1,10);
    size_t frame=manifest.audio.channels*(manifest.audio.bits/8);
    check(pcm==ss_capture_buffer+SS_CAPTURE_HEADROOM&&bytes==frames*frame,11);
    check(!memcmp(pcm,input+header+first*frame,bytes),12);
    for(unsigned i=0;i<SS_CAPTURE_HEADROOM;++i)check(ss_capture_buffer[i]==0xa5,13);
    for(size_t i=0;i<sizeof(input);++i)check(input[i]==(uint8_t)(i*17+7),14);
}
void audio_replay_run(unsigned selected)
{
    void *pcm=(void *)(uintptr_t)1;size_t bytes=123;
    reset(320,1,16,44);
    if(selected==0){
        check(ss_capture_replay_open(0,44,128,0)==1,20);
        payload(1,0,128,44);payload(1,0,128,44);payload(2,128,128,44);payload(3,256,64,44);
        check(ss_capture(4,&pcm,&bytes)==-1&&pcm==(void *)(uintptr_t)1&&bytes==123,21);
    }else if(selected==1){
        reset(320,1,16,0);check(ss_capture_replay_open(0,0,160,1),30);
        check(ss_capture(1,&pcm,&bytes)==0&&pcm==(void *)(uintptr_t)1&&bytes==123,31);
        counter=A733_SMP_COUNTER_HZ/100-1;check(ss_capture(1,&pcm,&bytes)==0,32);
        counter++;payload(1,0,160,0);
        check(ss_capture(2,&pcm,&bytes)==0&&ss_capture(1,&pcm,&bytes)==-1&&ss_capture(3,&pcm,&bytes)==-1,33);
        counter=2*A733_SMP_COUNTER_HZ/100;payload(2,160,160,0);
    }else if(selected==2){
        reset(200,1,16,0);manifest.audio.min_frames=100;
        check(ss_capture_replay_open(0,0,128,0),40);payload(1,0,128,0);
        check(ss_capture(2,&pcm,&bytes)==-1&&pcm==(void *)(uintptr_t)1&&bytes==123,41);
    }else if(selected==3){
        check(!ss_capture_replay_open(1,0,128,0),50);
        check(!ss_capture_replay_open(0,0,128,2)&&!ss_capture_replay_open(0,0,0,0),51);
        check(!ss_capture_replay_open(0,0,1025,0)&&!ss_capture_replay_open(0,0,1024,0),52);
        check(!ss_capture_replay_open(0,UINT64_MAX,128,0)&&!ss_capture_replay_open(0,1,128,0),53);
        grant.devices=SS_DEVICE_AUDIO_I2S;check(!ss_capture_replay_open(0,44,128,0),54);grant.devices=0;
        grant.regions[0].base=ss_capture_buffer;check(!ss_capture_replay_open(0,44,128,0),55);
        grant.regions[0].base=(void *)(UINTPTR_MAX-3);check(!ss_capture_replay_open(0,44,128,0),56);
        grant.regions[0].base=NULL;check(!ss_capture_replay_open(0,44,128,0),57);
        grant.regions[0].base=input;manifest.regions[0].kind=SS_REGION_WORK;check(!ss_capture_replay_open(0,44,128,0),58);
        manifest.regions[0].kind=SS_REGION_INPUT;manifest.regions[0].access=0;check(!ss_capture_replay_open(0,44,128,0),59);
        manifest.regions[0].access=SS_ACCESS_READ;granted=0;check(!ss_capture_replay_open(0,44,128,0),60);granted=1;
        active=0;check(!ss_capture_replay_open(0,44,128,0),61);active=1;
        check(ss_capture_replay_open(0,44,128,0)&&!ss_capture_replay_open(0,44,128,0),62);
        check(ss_capture(0,&pcm,&bytes)==-1&&ss_capture(1,NULL,&bytes)==-1&&ss_capture(1,&pcm,NULL)==-1,63);
    }else if(selected==4){
        const unsigned bits[]={16,24,32};
        for(unsigned i=0;i<3;++i)for(unsigned channels=1;channels<=2;++channels){
            reset(320,channels,bits[i],44);check(ss_capture_replay_open(0,44,128,0),70);
            payload(1,0,128,44);payload(2,128,128,44);payload(3,256,64,44);
        }
    }else if(selected==5){
        check(ss_capture_replay_open(0,44,128,1),80);counter=100;
        check(ss_capture(1,&pcm,&bytes)==0,81);counter=99;
        check(ss_capture(1,&pcm,&bytes)==-1,82);counter=A733_SMP_COUNTER_HZ;
        check(ss_capture(1,&pcm,&bytes)==-1&&pcm==(void *)(uintptr_t)1&&bytes==123,83);
    }else if(selected==6){
        check(ss_capture(1,&pcm,&bytes)==-1,90);
        check(ss_capture_replay_open(0,44,128,0),91);active=0;
        check(ss_capture(1,&pcm,&bytes)==-1&&pcm==(void *)(uintptr_t)1&&bytes==123,92);
    }else if(selected==7){
        reset(51,1,16,0);manifest.audio.rate_hz=44100;
        check(ss_capture_replay_open(0,0,17,1),100);check(ss_capture(1,&pcm,&bytes)==0,101);
        counter=(17*(uint64_t)A733_SMP_COUNTER_HZ+44099)/44100-1;
        check(ss_capture(1,&pcm,&bytes)==0,102);counter++;payload(1,0,17,0);
        /* A late consumer catches up to the original recording clock. */
        counter=(51*(uint64_t)A733_SMP_COUNTER_HZ+44099)/44100;
        payload(2,17,17,0);payload(3,34,17,0);
    }else check(0,200);
    audio_replay_result=1;
}
#ifdef AUDIO_REPLAY_HOST
int main(void){for(unsigned i=0;i<8;++i){audio_replay_run(i);check(audio_replay_result==1,201);}return 0;}
#endif
