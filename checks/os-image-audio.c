/* Native OS capture and stream state machines against modeled DMA replies,
 * audio-parent readiness and session admission. Provider construction remains
 * at the existing fixture boundary; capture receives the real stream API. */
#include "../apps/os/audio.c"
#include "../apps/os/application.h"
#define AUDIO_STATE (*audio)
#include "os-image-audio-model.h"
enum { CAPTURE_MAX_FRAMES=80000, CAPTURE_BYTES=2*CAPTURE_MAX_FRAMES };
uint8_t os_check_pcm[2][CAPTURE_BYTES];
_Alignas(4) uint8_t ss_capture_buffer[SS_CAPTURE_HEADROOM+40*OS_AUDIO_PERIOD_CAPACITY];
const size_t ss_capture_capacity=sizeof(ss_capture_buffer)-SS_CAPTURE_HEADROOM;
static const struct ss_manifest manifest={.audio={16000,1,16,16000,CAPTURE_MAX_FRAMES}};
const struct ss_application ss_application={.manifest=&manifest};
static struct os_session session;
static const struct ss_grant grant={.devices=SS_DEVICE_AUDIO_I2S};
const struct ss_grant *ss_grant(void){return &grant;}
int ss_live(void){return board_up&&!board_failed?1:-1;}
const struct os_session *os_session_admitted(void){return &session;}
int os_granted(unsigned mask){return mask&&(session.granted&mask)==mask;}
/* The provider, pin and DMA-pool constructors are outside this model, as in
 * the earlier stream fixture. Preserve the actual client's validation and
 * stream state machine while supplying their configured provider boundary. */
static unsigned stream_opens;
int __wrap_os_audio_stream_open(const struct os_audio_stream_config *c){++stream_opens;return configure(c);}
/* Mode 5: candidate rates and whether the stream accepts each one for
 * 16-bit mono capture with 4,096-byte periods; the runner compares them with
 * the vendor's table. */
const uint32_t os_check_rates[]={7999,8000,11025,12000,16000,22050,24000,32000,44100,48000,
    64000,88200,96000,128000,176400,192000,192001,384000};
uint8_t os_check_accepted[sizeof(os_check_rates)/sizeof(os_check_rates[0])];
static _Alignas(8) uint8_t ring_buffer[OS_AUDIO_RING_CAPACITY*OS_AUDIO_PERIOD_CAPACITY];

static void reset(void)
{
    /* The stream's state, which the image takes from the ledger at its first open (outside this model). */
    static struct os_audio_state state;
    audio=&state;memset(audio,0,sizeof(*audio));
    phase=DMA_SERVICE_CLOSED;produced=puts_done=reaps=closes=opens=silent=logged=0;
    sent=held_ticket=0;board_up=1;board_failed=0;parent_polls=0;
    for(unsigned i=0;i<PERIODS;++i)audio->pool.stream_mappings[i].cpu=memory[i];
    atomic_store(&audio->pool.state,A733_DMA_POOL_READY);
}
static struct os_audio_stream_config config(uint32_t rate,unsigned channels,unsigned bits,
    uint32_t period_bytes,uint32_t periods)
{return (struct os_audio_stream_config){{rate,(uint8_t)channels,(uint8_t)bits,{0}},period_bytes,periods,ring_buffer,0};}
static unsigned count(char op){unsigned n=0;for(unsigned i=0;i<logged;++i)n+=log[i]==op;return n;}
/* Stream task steps and adapter requests in turn, until generation g is
 * captured: 1, or -1 once the adapter refuses. */
static int capture_run(unsigned g,void **pcm,size_t *bytes)
{
    for(unsigned n=0;n<2000;++n){
        int r=ss_capture(g,pcm,bytes);if(r)return r;
        if(step()<0){fail();}
    }
    return 0;
}
static void check_pcm(unsigned g,unsigned frames,const void *data,size_t bytes)
{
    const uint8_t *pcm=data;
    check(bytes==2*frames&&pcm==ss_capture_buffer+SS_CAPTURE_HEADROOM&&audio->client.buffer==pcm,30);
    for(unsigned i=0;i<SS_CAPTURE_HEADROOM;++i)check(ss_capture_buffer[i]==0xa5,31);
    for(unsigned i=0;i<2*frames;++i)check(pcm[i]==sample(g,i/4096,i%4096),32);
    memcpy(os_check_pcm[g-1],pcm,bytes);
}
/* One run of a stream configuration through the generic interface: read and
 * release 2 * periods periods, check their place, order, timestamps and
 * bytes, stop, and start a second run that begins at slot 0 again. */
static void delivery(struct os_audio_stream_config c,unsigned frame)
{
    reset();check(configure(&c),100);
    model_bytes=c.period_bytes;model_frame=frame;
    unsigned frames=c.period_bytes/frame;
    struct os_audio_period p;
    for(unsigned run=1;run<=2;++run){
        check(os_audio_stream_start()==1,101);
        for(unsigned k=0;k<2*c.periods;++k){
            unsigned n=0;int r;
            while(!(r=os_audio_stream_read(&p))){check(step()==0&&++n<20,102);}
            check(r==1&&p.data==c.buffer+(size_t)(k%c.periods)*c.period_bytes&&p.bytes==c.period_bytes&&
                p.frames==frames&&p.first_frame==(uint64_t)k*frames&&
                p.observed_before_us==model_before(k)&&p.observed_after_us==model_after(k),103);
            for(unsigned i=0;i<c.period_bytes;++i)check(p.data[i]==sample(run,k,i),104);
            check(os_audio_stream_release()==1&&os_audio_stream_release()==-1,105);
        }
        check(os_audio_stream_stop()==0,106);
        unsigned n=0;
        while(os_audio_stream_stop()!=1)check(step()==0&&++n<20,107);
        check(phase==DMA_SERVICE_RESULT&&!board_failed&&opens==run,108);
    }
    check(reaps==1&&closes==1&&count('G')==count('P'),109);
}
void os_check_run(unsigned selected,unsigned samples)
{
    mode=selected;reset();
    void *pcm;size_t bytes;
    if(mode==11){
        /* A session that does not grant the I2S service: the start entry succeeds without opening the stream, which is
         * the only path to the pins, the clocks, the DMA channel and the interrupt. The device bit and the record are the
         * grant (apps/os/session.c); neither is present. */
        session.audio.capture_samples=samples;session.granted=0;
        stream_opens=0;check(os_capture_start()==1&&stream_opens==0,150);
        check(ss_capture(1,&pcm,&bytes)<0,151);
        session.granted=OS_GRANT_AUDIO;
        check(os_capture_start()==1&&stream_opens==1,152);
        os_check_result=1;return;
    }
    if(mode==5){
        /* Rates: exactly those whose bit clock divides 24.576 MHz. */
        for(unsigned i=0;i<sizeof(os_check_rates)/sizeof(os_check_rates[0]);++i){
            struct os_audio_stream_config c=config(os_check_rates[i],1,16,4096,4);
            reset();os_check_accepted[i]=(uint8_t)configure(&c);
        }
        /* Formats, periods and buffers the stream refuses. */
        struct os_audio_stream_config bad[]={config(16000,3,16,4096,4),config(16000,1,24,4096,4),
            config(16000,0,16,4096,4),config(16000,1,16,0,4),config(16000,1,16,4100,4),
            config(16000,1,16,4098,4),config(16000,1,16,1000,4),config(48000,2,16,1002,4),
            config(48000,2,32,4088,4),config(16000,1,16,4096,1),
            config(16000,1,16,4096,OS_AUDIO_RING_CAPACITY+1),config(16000,1,16,4096,4),
            config(16000,1,16,4096,4),config(16000,1,16,4096,4),config(44100,2,16,4096,4),
            /* Shorter than 8 ms: 7 ms at 8 kHz, 5.3 ms at 192 kHz. */
            config(8000,1,16,112,2),config(192000,2,32,4096,8),config(192000,1,32,4096,8)};
        bad[11].buffer=NULL;bad[12].buffer=ring_buffer+2;bad[13].format.reserved[1]=1;
        for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);++i){reset();check(!configure(&bad[i]),90+i);}
        struct os_audio_stream_config inside=config(16000,1,16,4096,2);
        inside.buffer=(uint8_t *)audio;reset();check(!configure(&inside),110);
        /* Valid edges: the capacity, the most slots, small stereo periods. */
        struct os_audio_stream_config good[]={config(16000,1,16,4096,OS_AUDIO_RING_CAPACITY),
            config(8000,1,16,128,2),config(192000,1,16,4096,8),config(96000,2,16,4096,3)};
        for(unsigned i=0;i<sizeof(good)/sizeof(good[0]);++i){reset();check(configure(&good[i]),111+i);}
        /* Configured once only. */
        check(!configure(&good[0]),115);
        os_check_result=1;return;
    }
    if(mode==6){
        /* Rates and period sizes through the generic interface. */
        delivery(config(8000,1,16,512,2),2);
        delivery(config(16000,1,16,4096,4),2);
        delivery(config(16000,1,32,2048,5),4);
        delivery(config(48000,2,16,2048,3),4);
        delivery(config(96000,1,32,4096,6),4);
        delivery(config(192000,1,16,4096,8),2);
        os_check_result=1;return;
    }
    if(mode==8||mode==9||mode==10){
        struct os_audio_stream_config c=config(16000,1,16,4096,4);c.cyclic=1;
        if(mode==8){delivery(c,2);os_check_result=1;return;}
        check(configure(&c)&&os_audio_stream_start()==1,140);
        int r=0;for(unsigned i=0;i<20&&r==0;++i)r=step();
        check(r<0&&held_ticket&&!puts_done&&!reaps&&!closes,141);
        fail();struct os_audio_period p;check(os_audio_stream_read(&p)<0,142);
        os_check_result=1;return;
    }
    if(mode==7){
        /* A full ring drops periods; the next delivered period shows the
         * gap. Release follows read order; start and stop handshakes. */
        struct os_audio_stream_config c=config(16000,1,16,4096,2);
        check(configure(&c),120);
        struct os_audio_period p;
        check(os_audio_stream_release()==-1&&os_audio_stream_read(&p)==0&&os_audio_stream_stop()==1,121);
        check(os_audio_stream_start()==1&&os_audio_stream_start()==1,122);
        for(unsigned n=0;n<40&&produced<6;++n)check(step()==0,123);
        check(produced==6&&atomic_load(&audio->written)==2,124);
        check(os_audio_stream_read(&p)==1&&p.first_frame==0&&os_audio_stream_read(&p)==1&&p.first_frame==2048,125);
        check(os_audio_stream_read(&p)==0&&os_audio_stream_release()==1,126);
        while(produced<8)check(step()==0,127);
        unsigned gap=0;int r;
        while(!(r=os_audio_stream_read(&p)))check(step()==0&&++gap<20,128);
        check(r==1&&p.first_frame>=6*2048&&p.first_frame%2048==0&&p.data==ring_buffer,129);
        for(unsigned i=0;i<4096;++i)check(p.data[i]==sample(1,(unsigned)(p.first_frame/2048),i),130);
        /* A stop in progress refuses a new start until the DMA drained. */
        check(os_audio_stream_stop()==0&&os_audio_stream_start()==0,131);
        while(os_audio_stream_stop()!=1)check(step()==0,132);
        check(!board_failed&&count('C')==1,133);
        /* Stopped before the stream task saw the start: no DMA request. */
        unsigned before=logged;
        check(os_audio_stream_start()==1&&os_audio_stream_stop()==0,134);
        for(unsigned n=0;n<5;++n)check(step()==0,135);
        check(os_audio_stream_stop()==1&&logged==before,136);
        os_check_result=1;return;
    }
    session.audio.capture_samples=samples;session.granted=OS_GRANT_AUDIO;
    memset(ss_capture_buffer,0xa5,SS_CAPTURE_HEADROOM);
    check(os_capture_start()&&!os_capture_start(),20);
    if(mode==4){
        /* Each task pass without an unanswered request reports progress; a
         * pass waiting for the worker or after a failure reports none. */
        for(unsigned n=0;n<10;++n)pass();
        check(audio->stage==IDLE&&reports==REPORTS(10),70);
        check(!ss_capture(1,&pcm,&bytes),71);
        pass();check(audio->stage==OPEN&&!audio->pending&&reports==REPORTS(11),72);
        silent=1;pass();pass();
        check(audio->stage==OPEN&&audio->pending&&reports==REPORTS(11),73);
        silent=0;pass();
        check(audio->stage==OPENING&&!audio->pending&&reports==REPORTS(12),74);
        board_up=0;pass();pass();
        check(board_failed&&atomic_load(&audio->failed)&&reports==REPORTS(12),75);
        os_check_result=1;return;
    }
    if(mode==3){
        check(os_audio_client_admit(16000)&&os_audio_client_admit(CAPTURE_MAX_FRAMES)&&
            !os_audio_client_admit(15999)&&!os_audio_client_admit(CAPTURE_MAX_FRAMES+1),43);
        check(ss_capture(0,&pcm,&bytes)<0&&ss_capture(1,NULL,&bytes)<0,40);
        check(!ss_capture(2,&pcm,&bytes)&&ss_capture(1,&pcm,&bytes)<0,41);
        board_up=0;check(ss_capture(2,&pcm,&bytes)<0,42);
        os_check_result=1;return;
    }
    /* The parent becomes ready on the third poll; nothing is requested yet. */
    for(unsigned n=0;n<10;++n)check(step()==0,50);
    check(atomic_load(&audio->ready)&&audio->stage==IDLE&&!logged,51);
    int r=capture_run(1,&pcm,&bytes);
    if(mode){
        /* A discontinuous period or a stream that stopped by itself refuses
         * the capture with the stream and its leases held. */
        check(r<0&&board_failed&&!reaps&&!closes&&ss_capture(1,&pcm,&bytes)<0,60+mode);
        os_check_result=1;return;
    }
    unsigned periods=(2*samples+4095)/4096;
    check(r==1&&produced>=periods&&count('G')==produced&&count('P')==produced&&count('C')==1,53);
    check(!memcmp(log,"OSM",3)&&!memcmp(log+logged-2,"CS",2)&&!reaps,54);
    check_pcm(1,samples,pcm,bytes);
    /* The next generation reaps, closes and opens again before it captures;
     * the first generation's request is then refused. */
    unsigned first=logged;
    check(!ss_capture(2,&pcm,&bytes)&&capture_run(2,&pcm,&bytes)==1&&reaps==1&&closes==1,55);
    check(!memcmp(log+first,"RLSOSM",6)&&!memcmp(log+logged-2,"CS",2),56);
    check_pcm(2,samples,pcm,bytes);
    check(ss_capture(1,&pcm,&bytes)<0&&!board_failed,57);
    os_check_result=1;
}
