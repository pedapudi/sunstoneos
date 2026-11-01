/* The integrated OS image's capture stream (apps/os/audio.c) and the
 * recognizer's WAV adapter (apps/mimus-board/capture.c, linked beside this
 * unit) against a modeled DMA service, audio parent and board. The stream's
 * own state machine, request protocol, ring delivery and resident progress
 * reports run unchanged; the runner (checks/os-image-audio.py) executes each
 * mode under unicorn. */
#include "../apps/os/audio.c"
#include "../apps/mimus-board/capture.h"
#include "os-image-audio-model.h"
/* WAV output of the first two generations, for the runner's comparison with
 * the capture client of the baseline revision. */
uint8_t os_check_wav[2][44+2*MIMUS_CAPTURE_MAX_SAMPLES];
/* Mode 5: candidate rates and whether the stream accepts each one for
 * 16-bit mono capture with 4,096-byte periods; the runner compares them with
 * the vendor's table. */
const uint32_t os_check_rates[]={7999,8000,11025,12000,16000,22050,24000,32000,44100,48000,
    64000,88200,96000,128000,176400,192000,192001,384000};
uint8_t os_check_accepted[sizeof(os_check_rates)/sizeof(os_check_rates[0])];
static _Alignas(8) uint8_t ring_buffer[OS_AUDIO_RING_CAPACITY*OS_AUDIO_PERIOD_CAPACITY];

static void reset(void)
{
    memset(&audio,0,sizeof(audio));
    phase=DMA_SERVICE_CLOSED;produced=puts_done=reaps=closes=opens=silent=logged=0;
    sent=held_ticket=0;board_up=1;board_failed=0;parent_polls=0;
    for(unsigned i=0;i<PERIODS;++i)audio.pool.stream_mappings[i].cpu=memory[i];
    atomic_store(&audio.pool.state,A733_DMA_POOL_READY);
}
static struct os_audio_stream_config config(uint32_t rate,unsigned channels,unsigned bits,
    uint32_t period_bytes,uint32_t periods)
{return (struct os_audio_stream_config){{rate,(uint8_t)channels,(uint8_t)bits,{0}},period_bytes,periods,ring_buffer};}
static unsigned count(char op){unsigned n=0;for(unsigned i=0;i<logged;++i)n+=log[i]==op;return n;}
static uint32_t get32(const uint8_t *p){return p[0]|p[1]<<8|p[2]<<16|(uint32_t)p[3]<<24;}
/* Stream task steps and adapter requests in turn, until generation g is
 * captured: 1, or -1 once the adapter refuses. */
static int capture_run(unsigned g,const uint8_t **wav,size_t *bytes)
{
    for(unsigned n=0;n<2000;++n){
        int r=mimus_capture(g,wav,bytes);if(r)return r;
        if(step()<0){fail();}
    }
    return 0;
}
static void check_wav(unsigned g,unsigned samples,const uint8_t *wav,size_t bytes)
{
    check(bytes==44+2*samples,30);
    check(!memcmp(wav,"RIFF",4)&&get32(wav+4)==36+2*samples&&!memcmp(wav+8,"WAVEfmt ",8)&&
        get32(wav+16)==16&&get32(wav+20)==0x10001&&get32(wav+24)==16000&&get32(wav+28)==32000&&
        get32(wav+32)==0x100002&&!memcmp(wav+36,"data",4)&&get32(wav+40)==2*samples,31);
    for(unsigned i=0;i<2*samples;++i)check(wav[44+i]==sample(g,i/4096,i%4096),32);
    memcpy(os_check_wav[g-1],wav,bytes);
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
    const uint8_t *wav;size_t bytes;
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
        inside.buffer=(uint8_t *)&audio;reset();check(!configure(&inside),110);
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
    if(mode==7){
        /* A full ring drops periods; the next delivered period shows the
         * gap. Release follows read order; start and stop handshakes. */
        struct os_audio_stream_config c=config(16000,1,16,4096,2);
        check(configure(&c),120);
        struct os_audio_period p;
        check(os_audio_stream_release()==-1&&os_audio_stream_read(&p)==0&&os_audio_stream_stop()==1,121);
        check(os_audio_stream_start()==1&&os_audio_stream_start()==1,122);
        for(unsigned n=0;n<40&&produced<6;++n)check(step()==0,123);
        check(produced==6&&atomic_load(&audio.written)==2,124);
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
    const struct os_audio_stream_config *c=mimus_capture_configure(samples);
    check(c&&configure(c)&&!mimus_capture_configure(samples),20);
    if(mode==4){
        /* Each task pass without an unanswered request reports progress; a
         * pass waiting for the worker or after a failure reports none. */
        for(unsigned n=0;n<10;++n)pass();
        check(audio.stage==IDLE&&reports==REPORTS(10),70);
        check(!mimus_capture(1,&wav,&bytes),71);
        pass();check(audio.stage==OPEN&&!audio.pending&&reports==REPORTS(11),72);
        silent=1;pass();pass();
        check(audio.stage==OPEN&&audio.pending&&reports==REPORTS(11),73);
        silent=0;pass();
        check(audio.stage==OPENING&&!audio.pending&&reports==REPORTS(12),74);
        board_up=0;pass();pass();
        check(board_failed&&atomic_load(&audio.failed)&&reports==REPORTS(12),75);
        os_check_result=1;return;
    }
    if(mode==3){
        check(mimus_capture(0,&wav,&bytes)<0&&mimus_capture(1,NULL,&bytes)<0,40);
        check(!mimus_capture(2,&wav,&bytes)&&mimus_capture(1,&wav,&bytes)<0,41);
        board_up=0;check(mimus_capture(2,&wav,&bytes)<0,42);
        os_check_result=1;return;
    }
    /* The parent becomes ready on the third poll; nothing is requested yet. */
    for(unsigned n=0;n<10;++n)check(step()==0,50);
    check(atomic_load(&audio.ready)&&audio.stage==IDLE&&!logged,51);
    int r=capture_run(1,&wav,&bytes);
    if(mode){
        /* A discontinuous period or a stream that stopped by itself refuses
         * the capture with the stream and its leases held. */
        check(r<0&&board_failed&&!reaps&&!closes&&mimus_capture(1,&wav,&bytes)<0,60+mode);
        os_check_result=1;return;
    }
    unsigned periods=(2*samples+4095)/4096;
    check(r==1&&produced>=periods&&count('G')==produced&&count('P')==produced&&count('C')==1,53);
    check(!memcmp(log,"OSM",3)&&!memcmp(log+logged-2,"CS",2)&&!reaps,54);
    check_wav(1,samples,wav,bytes);
    /* The next generation reaps, closes and opens again before it captures;
     * the first generation's request is then refused. */
    unsigned first=logged;
    check(!mimus_capture(2,&wav,&bytes)&&capture_run(2,&wav,&bytes)==1&&reaps==1&&closes==1,55);
    check(!memcmp(log+first,"RLSOSM",6)&&!memcmp(log+logged-2,"CS",2),56);
    check_wav(2,samples,wav,bytes);
    check(mimus_capture(1,&wav,&bytes)<0&&!board_failed,57);
    os_check_result=1;
}
