/* Actual native stack and pinned codec; registers/peer/HCI effects modeled. */
#include <stdint.h>
static unsigned msbc_no_cap,msbc_bad_bcs,msbc_fallback,msbc_refuse,msbc_setups,msbc_selected;
/* Segments hold two frames instead of one, so one activation spans both. */
static unsigned msbc_two_frames;
/* The headset drops mSBC from its codec list while the owner waits idle. */
static unsigned msbc_narrowed;
/* The headset withholds its AT+BCS answer, holding a codec offer in flight. */
static unsigned msbc_hold_bcs;
#define BLUETOOTH_CHECK_MSBC 1
#define BLUETOOTH_CHECK_REPEATED 1
#define BLUETOOTH_HEADSET_FIXTURE_ENTRY archived_headset_main
#include "bluetooth-headset.c"
#undef BLUETOOTH_HEADSET_FIXTURE_ENTRY
#include <sbc/sbc.h>
static TaskHandle_t reader=(TaskHandle_t)2;
static const char *latency_codec="mSBC";
static void inject(const uint8_t *p,unsigned n,unsigned status)
{uint8_t h[]={3,0,(uint8_t)(status<<4),(uint8_t)n};assert(n<=255);bytes(&wire,h,4);bytes(&wire,p,n);}
static uint64_t segments(void){return atomic_load(&voice.segment_head);}
#define MSBC_UNTIL(condition) do{for(unsigned n_=0;n_<400&&!(condition);++n_)assert(pump()>=0);assert(condition);}while(0)
/* The HF supports codec negotiation and voice recognition. */
static void connect_audio(void)
{
    until(CONNECT);const uint8_t request[]={1,2,3,4,5,6,4,4,0x24,1};event(&wire,4,request,sizeof(request));
    until(CONTROL);const uint8_t connect[]={3,0,0x61,0};sig_in(2,50,connect,4);until(AG);
    const char *at="AT+BRSF=136\rAT+BAC=1,2\rAT+CIND=?\rAT+CIND?\rAT+CMER=3,,,1\r";
    rf_in(14,0xef,(const uint8_t *)at,(unsigned)strlen(at),-1);
}
/* An owner-started activation: +BVRA: 1, then the link, opened by the first. */
static void activate(void)
{
    current_task=reader;assert(bluetooth_headset_segment_request(&app,BLUETOOTH_SEGMENT_START,atomic_load(&voice.segment_head))==1);current_task=owner;
    LATENCY("START to committed on",POLLS(voice.ag.recognition),5);assert(voice.active);
    /* A codec list changed while idle adds the +BCS exchange before setup. */
    if(voice.stage!=CAPTURE){unsigned setups=sco_setups;
        if(msbc_narrowed)LATENCY("START commit to setup accepted after codec reselection",POLLS(sco_setups==setups+1),13);
        else LATENCY("START commit to setup accepted",POLLS(sco_setups==setups+1),4);}
    MSBC_UNTIL(voice.active&&voice.stage==CAPTURE);
}
/* The segment-ending activation's +BVRA: 0 has been accepted. */
static void ended(uint64_t published)
{
    MSBC_UNTIL(segments()==published&&!voice.active);
    MSBC_UNTIL(!voice.ag.recognition&&!voice.voice_off);
}
static struct bluetooth_segment_snapshot take(uint64_t ticket,FILE *out)
{
    current_task=reader;const uint8_t *pcm;struct bluetooth_segment_snapshot s;
    assert(bluetooth_headset_segment_take(&app,&pcm,&s)==1&&s.ticket==ticket&&s.capture.bytes==profile.headset.samples*2);
    uint8_t digest[32];storage_digest_bytes(pcm,s.capture.bytes,digest);assert(!memcmp(digest,s.capture.sha256,32));
    if(out)assert(fwrite(pcm,1,s.capture.bytes,out)==s.capture.bytes);
    assert(s.capture.state==(msbc_no_cap||msbc_fallback||msbc_narrowed?1u:2u));
    assert(bluetooth_headset_segment_ack(&app,ticket+1)==-1&&bluetooth_headset_segment_ack(&app,ticket)==1&&bluetooth_headset_segment_ack(&app,ticket)==-1);
    current_task=owner;return s;
}
static void close_audio(void)
{
    assert(bluetooth_headset_segment_stop(&app)==1);until(CLOSE);for(unsigned n=0;n<8;++n)assert(pump()>=0);
    assert(!atomic_load(&voice.segment_disconnected));const uint8_t done[]={0,0,0,0x13};event(&wire,5,done,sizeof(done));until(DONE);
}
static void encode(sbc_t *encoder,const uint8_t pcm[240],unsigned sequence,uint8_t frame[60])
{
    static const uint8_t h2[]={8,0x38,0xc8,0xf8};memset(frame,0,60);frame[0]=1;frame[1]=h2[sequence&3];ssize_t written=0;
    assert(sbc_encode(encoder,pcm,240,frame+2,57,&written)==240&&written==57);
}
/* From a running activation: two activations on one open link, separated by
 * three released frames, with 7-byte packets that split frames. The second
 * activation skips to the next frame boundary and decodes the first whole
 * frame after the gap. */
static void activations(sbc_t *encoder)
{
    enum {FRAMES=10};uint8_t stream[3+60*FRAMES]={0x74,0x22,0x00},pcm[240];
    for(unsigned k=0;k<FRAMES;++k){for(unsigned i=0;i<240;i+=2){pcm[i]=0;pcm[i+1]=(uint8_t)(k*9+i/16);}encode(encoder,pcm,k,stream+3+60*k);}
    unsigned at=0;assert(voice.active);
    while(!segments()){assert(at+7<=sizeof(stream));inject(stream+at,7,0);for(unsigned k=0;k<4;++k)assert(pump()>=0);at+=7;}
    assert(at==63&&!voice.active);ended(1);
    /* Frames 1 to 3 and 23 bytes of frame 4 are released without decoding. */
    while(at<3+60*4+23){inject(stream+at,7,0);for(unsigned k=0;k<4;++k)assert(pump()>=0);at+=7;}
    assert(at==266&&segments()==1&&!voice.segment_bytes);take(1,NULL);
    /* The released bytes end 23 bytes into frame 4, so the next activation
     * skips its other 37 bytes and decodes frame 5 without a header search. */
    activate();const unsigned next=3+60*5;assert(voice.skip==37&&voice.boundary==63);
    while(segments()<2){assert(at+7<=next+60+6);inject(stream+at,7,0);for(unsigned k=0;k<4;++k)assert(pump()>=0);at+=7;}
    assert(at>=next+60&&at-7<next+60&&!atomic_load(&voice.segment_flags));take(2,NULL);
}
#ifndef BLUETOOTH_MSBC_FIXTURE_ENTRY
#define BLUETOOTH_MSBC_FIXTURE_ENTRY main
#endif
int BLUETOOTH_MSBC_FIXTURE_ENTRY(int argc,char **argv)
{
    assert(argc==2||argc==4);speech=!strcmp(argv[1],"speech");msbc_no_cap=!strcmp(argv[1],"no-capability");
    msbc_bad_bcs=!strcmp(argv[1],"bad-bcs");msbc_fallback=!strcmp(argv[1],"fallback");msbc_refuse=!strcmp(argv[1],"command-refusal");
    msbc_two_frames=!strcmp(argv[1],"sequence");assert(bluetooth_msbc_init()==1);setup();scheduler=taskSCHEDULER_NOT_STARTED;
    assert(bluetooth_headset_segment_bind(&app,reader)==1);scheduler=taskSCHEDULER_RUNNING;connect_audio();
    if(msbc_bad_bcs){for(unsigned n=0;n<1000;++n)assert(pump()>=0);assert(!msbc_setups&&!voice.ag.selected_codec);
        wire.now=voice.ag.codec_deadline;assert(pump()<0);puts("PASS mismatched BCS cannot start synchronous setup");return 0;}
    until(IDLE);assert(!msbc_setups&&brsf==BT_HFP_AG_VOICE+(voice.wideband?512u:0u)+call_features);
    if(msbc_refuse){current_task=reader;assert(bluetooth_headset_segment_request(&app,BLUETOOTH_SEGMENT_START,atomic_load(&voice.segment_head))==1);current_task=owner;
        for(unsigned n=0;n<2000&&!atomic_load(&app.failed);++n)if(pump()<0)break;
        assert(atomic_load(&app.failed)&&msbc_setups==1&&!atomic_load(&voice.segment_disconnected));puts("PASS command refusal retains without speculative fallback");return 0;}
    if(!strcmp(argv[1],"bac-idle")){
        /* AT+BAC while idle clears the mSBC selection; the first activation
         * selects CVSD again before it opens the link. */
        const char *narrow="AT+BAC=1\r";rf_in(14,0xef,(const uint8_t *)narrow,(unsigned)strlen(narrow),-1);
        for(unsigned n=0;n<100;++n)assert(pump()>=0);
        assert(voice.stage==IDLE&&!voice.ag.selected_codec&&voice.ag.peer_codecs==2&&!msbc_setups);msbc_narrowed=1;
        activate();assert(stream()->codec==1&&voice.ag.selected_codec==1&&msbc_setups==1);
        uint8_t pcm[240]={0};inject(pcm,sizeof(pcm),0);MSBC_UNTIL(segments()==1);ended(1);take(1,NULL);close_audio();
        puts("PASS a codec list changed while idle is negotiated again before the link opens");return 0;
    }
    if(!strcmp(argv[1],"button")){
        /* The headset button opens the mSBC link; one whole frame in one
         * packet fills the segment. */
        current_task=reader;assert(bluetooth_headset_segment_request(&app,BLUETOOTH_SEGMENT_ACCEPT,segments())==1);current_task=owner;
        for(unsigned n=0;n<2;++n)assert(pump()>=0);
        rf_in(14,0xef,(const uint8_t *)"AT+BVRA=1\r",10,-1);LATENCY("AT+BVRA=1 line to committed on",POLLS(voice.ag.recognition),4);
        unsigned setups=sco_setups;LATENCY("AT+BVRA=1 commit to setup accepted",POLLS(sco_setups==setups+1),4);
        MSBC_UNTIL(voice.active&&voice.stage==CAPTURE);assert(stream()->codec==2);
        sbc_t encoder;assert(!sbc_init_msbc(&encoder,0));encoder.endian=SBC_LE;uint8_t pcm[240]={0},frame[60];encode(&encoder,pcm,0,frame);
        unsigned off=bvra_off;inject(frame,60,0);LATENCY("packet to first PCM byte in a slot",POLLS(voice.segment_bytes||segments()),1);
        LATENCY("publication to +BVRA: 0 queued",POLLS(voice.ag.response_length||bvra_off!=off),1);
        ended(1);take(1,NULL);close_audio();sbc_finish(&encoder);
        puts("PASS the headset button records one mSBC segment");return 0;
    }
    if(!strcmp(argv[1],"ag-cancel")){
        /* The headset cancels the first activation while the AG stage
         * reselects the codec, and a stop follows: the AG stage ends like
         * idle once the offer is answered, without retention or a link. */
        const char *narrow="AT+BAC=1\r";rf_in(14,0xef,(const uint8_t *)narrow,(unsigned)strlen(narrow),-1);
        for(unsigned n=0;n<100;++n)assert(pump()>=0);
        msbc_narrowed=1;msbc_hold_bcs=1;
        current_task=reader;assert(bluetooth_headset_segment_request(&app,BLUETOOTH_SEGMENT_START,segments())==1);current_task=owner;
        MSBC_UNTIL(voice.active&&voice.stage==AG&&voice.ag.offered_codec);
        rf_in(14,0xef,(const uint8_t *)"AT+BVRA=0\r",10,-1);MSBC_UNTIL(!voice.active);assert(voice.stage==AG);
        current_task=reader;assert(bluetooth_headset_segment_stop(&app)==1);current_task=owner;
        for(unsigned n=0;n<20;++n)assert(pump()>=0);
        assert(voice.stage==AG&&!atomic_load(&app.failed)&&!voice.ag.voice_admit);
        msbc_hold_bcs=0;rf_in(14,0xef,(const uint8_t *)"AT+BCS=1\r",9,-1);MSBC_UNTIL(voice.stage==DONE);
        assert(atomic_load(&voice.segment_disconnected)&&!atomic_load(&app.failed)&&!(atomic_load(&recovery)&4)&&
            !msbc_setups&&!segments()&&atomic_load(&voice.segment_flags)==BLUETOOTH_SEGMENT_CANCEL);
        puts("PASS a stop after a cancel during codec reselection ends the AG stage without retention");return 0;
    }
    activate();assert(stream()->codec==(msbc_no_cap||msbc_fallback?1u:2u));
    if(!strcmp(argv[1],"cadence")){
        /* mSBC silence follows the 7.5 ms eSCO frame period on a jittered
         * poll grid, during an activation and between activations. */
        for(unsigned phase=0;phase<2;++phase){
            uint64_t start=wire.now,before=sco_bytes;jittered(900000);
            uint64_t frames=(sco_bytes-before)/60,expected=(wire.now-start)/7500;
            fprintf(stderr,"mSBC silence %llu frames over %llu us, expected %llu\n",(unsigned long long)frames,(unsigned long long)(wire.now-start),(unsigned long long)expected);
            assert(frames<=expected+1&&frames+1>=expected&&!voice.silence_restarts);
            if(!phase){rf_in(14,0xef,(const uint8_t *)"AT+BVRA=0\r",10,-1);MSBC_UNTIL(!voice.active);}
        }
        puts("PASS mSBC silence cadence anchored to its frame period");return 0;
    }
    if(!strcmp(argv[1],"bac")){
        /* HFP lets the HF resend AT+BAC during audio. A list that keeps mSBC
         * keeps the selection; one that drops it requires reselection only. */
        const char *same="AT+BAC=1,2\r",*narrow="AT+BAC=1\r";
        rf_in(14,0xef,(const uint8_t *)same,(unsigned)strlen(same),-1);for(unsigned n=0;n<100;++n)assert(pump()>=0);
        assert(voice.ag.peer_codecs==6&&voice.ag.selected_codec==2&&stream()->phase==BT_SCO_OPEN);
        rf_in(14,0xef,(const uint8_t *)narrow,(unsigned)strlen(narrow),-1);for(unsigned n=0;n<100;++n)assert(pump()>=0);
        assert(voice.ag.peer_codecs==2&&!voice.ag.selected_codec&&stream()->phase==BT_SCO_OPEN&&stream()->codec==2);
    }
    if(msbc_fallback)assert(msbc_setups==3&&voice.stream==2&&voice.ag.selected_codec==1);
    if(msbc_no_cap||msbc_fallback){uint8_t pcm[240]={0};inject(pcm,sizeof(pcm),0);MSBC_UNTIL(segments()==1);ended(1);take(1,NULL);close_audio();puts("PASS explicit CVSD fallback identity");return 0;}
    sbc_t encoder;assert(!sbc_init_msbc(&encoder,0));encoder.endian=SBC_LE;
    if(!strcmp(argv[1],"activations")){activations(&encoder);close_audio();sbc_finish(&encoder);
        puts("PASS a resynchronized activation after an odd number of released frames decodes the next whole frame");return 0;}
    uint8_t input[96000]={0};unsigned total=speech?sizeof(input):!strcmp(argv[1],"backlog")||msbc_two_frames?480:240;
    FILE *out=NULL;if(speech){assert(argc==4);FILE *in=fopen(argv[2],"rb");out=fopen(argv[3],"wb");assert(in&&out&&fread(input,1,sizeof(input),in)==sizeof(input)&&fgetc(in)==EOF&&!fclose(in));}
    unsigned seq=0,taken=0;uint64_t published=0;struct bluetooth_segment_snapshot snapshots[2];
    for(unsigned off=0;off<total;off+=240){uint8_t frame[60];encode(&encoder,input+off,seq++,frame);
        if(!strcmp(argv[1],"crc"))frame[5]^=1;
        /* Within one activation the H2 sequence must advance. */
        if(msbc_two_frames&&off)frame[1]=0x08;
        if(!strcmp(argv[1],"loss")){inject(frame,60,2);}else {
            /* Initial alignment and every header/frame are split across HCI records. */
            if(!off){const uint8_t prefix[]={0x74,0x22,0x00};inject(prefix,3,0);for(unsigned n=0;n<4;++n)assert(pump()>=0);}
            for(unsigned at=0;at<60;){unsigned n=(at%7)+1;if(n>60-at)n=60-at;inject(frame+at,n,0);for(unsigned k=0;k<4;++k)assert(pump()>=0);at+=n;}
        }
        if(!strcmp(argv[1],"crc")||!strcmp(argv[1],"loss")||(msbc_two_frames&&off)){
            until(CLOSE);assert(atomic_load(&voice.segment_flags)&BLUETOOTH_SEGMENT_LOSS);assert(!segments());close_audio();sbc_finish(&encoder);puts("PASS codec/sequence/HCI loss stops without publishing damaged PCM");return 0;}
        /* Each full segment ends its activation; the next one starts before
         * the following frame, so no frame is released between them. */
        if(segments()>published){
            ended(++published);
            if(strcmp(argv[1],"backlog")){assert(taken<2);snapshots[taken]=take(taken+1,out);++taken;}
            if(off+240<total)activate();
        }
    }
    if(!strcmp(argv[1],"backlog")){
        /* The third activation finds both slots borrowed and stops before it
         * writes. */
        activate();uint8_t frame[60];encode(&encoder,input,seq++,frame);inject(frame,60,0);
        until(CLOSE);assert(segments()==2&&(atomic_load(&voice.segment_flags)&BLUETOOTH_SEGMENT_BACKLOG));take(1,NULL);take(2,NULL);close_audio();sbc_finish(&encoder);puts("PASS full decoded ring retains accepted PCM and stops before overwrite");return 0;}
    assert(taken==(speech?2u:1u));close_audio();sbc_finish(&encoder);
    if(speech){assert(!fclose(out));char path[4096];snprintf(path,sizeof(path),"%s.snapshots.bin",argv[3]);FILE *f=fopen(path,"wb");assert(f&&fwrite(snapshots,1,sizeof(snapshots),f)==sizeof(snapshots)&&!fclose(f));}
    puts("PASS actual BAC/BCS→transparent eSCO→fragmented H2/libsbc→owned16k PCM→exact ACK/confirmed close");return 0;
}
#ifndef BLUETOOTH_CHECK_PLAYBACK
/* The legacy capture fixture never enters caller-PCM critical sections. Keep
 * the new optional owner's link dependencies explicit and fail if it does. */
void vPortEnterCritical(void){assert(0);}
void vPortExitCritical(void){assert(0);}
#endif
