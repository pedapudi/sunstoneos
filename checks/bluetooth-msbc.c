/* Actual native stack and pinned codec; registers/peer/HCI effects modeled. */
#include <stdint.h>
static unsigned msbc_no_cap,msbc_bad_bcs,msbc_fallback,msbc_refuse,msbc_setups,msbc_selected;
#define BLUETOOTH_CHECK_MSBC 1
#define BLUETOOTH_CHECK_REPEATED 1
#define BLUETOOTH_HEADSET_FIXTURE_ENTRY archived_headset_main
#include "bluetooth-headset.c"
#undef BLUETOOTH_HEADSET_FIXTURE_ENTRY
#include <sbc/sbc.h>
static TaskHandle_t reader=(TaskHandle_t)2;
static void inject(const uint8_t *p,unsigned n,unsigned status)
{uint8_t h[]={3,0,(uint8_t)(status<<4),(uint8_t)n};assert(n<=255);bytes(&wire,h,4);bytes(&wire,p,n);}
static void connect_audio(void)
{
    until(CONNECT);const uint8_t request[]={1,2,3,4,5,6,4,4,0x24,1};event(&wire,4,request,sizeof(request));
    until(CONTROL);const uint8_t connect[]={3,0,0x61,0};sig_in(2,50,connect,4);until(AG);
    const char *at="AT+BRSF=128\rAT+BAC=1,2\rAT+CIND=?\rAT+CIND?\rAT+CMER=3,,,1\r";
    rf_in(14,0xef,(const uint8_t *)at,(unsigned)strlen(at),-1);
}
static struct bluetooth_segment_snapshot take(uint64_t ticket,FILE *out)
{
    current_task=reader;uint8_t b[1024];size_t copied;struct bluetooth_segment_snapshot s,identity={0};
    for(unsigned at=0;at<profile.headset.samples*2;){unsigned n=profile.headset.samples*2-at;if(n>1024)n=1024;
        assert(bluetooth_headset_segment_copy(&app,ticket,at,n,b,&copied,&s)==1&&copied==n);
        if(!at)identity=s;else assert(!memcmp(&identity,&s,sizeof(s)));
        if(out)assert(fwrite(b,1,n,out)==n);at+=n;}
    assert(identity.capture.state==(msbc_no_cap||msbc_fallback?1u:2u));
    assert(bluetooth_headset_segment_ack(&app,ticket+1)==-1&&bluetooth_headset_segment_ack(&app,ticket)==1&&bluetooth_headset_segment_ack(&app,ticket)==-1);
    current_task=owner;return identity;
}
static void close_audio(void)
{
    assert(bluetooth_headset_segment_stop(&app)==1);until(CLOSE);for(unsigned n=0;n<8;++n)assert(pump()>=0);
    assert(!atomic_load(&voice.segment_disconnected));const uint8_t done[]={0,0,0,0x13};event(&wire,5,done,sizeof(done));until(DONE);
}
#ifndef BLUETOOTH_MSBC_FIXTURE_ENTRY
#define BLUETOOTH_MSBC_FIXTURE_ENTRY main
#endif
int BLUETOOTH_MSBC_FIXTURE_ENTRY(int argc,char **argv)
{
    assert(argc==2||argc==4);speech=!strcmp(argv[1],"speech");msbc_no_cap=!strcmp(argv[1],"no-capability");
    msbc_bad_bcs=!strcmp(argv[1],"bad-bcs");msbc_fallback=!strcmp(argv[1],"fallback");msbc_refuse=!strcmp(argv[1],"command-refusal");
    assert(bluetooth_msbc_init()==1);setup();scheduler=taskSCHEDULER_NOT_STARTED;
    assert(bluetooth_headset_segment_bind(&app,reader)==1);scheduler=taskSCHEDULER_RUNNING;connect_audio();
    if(msbc_bad_bcs){for(unsigned n=0;n<1000;++n)assert(pump()>=0);assert(!msbc_setups&&!voice.ag.selected_codec);
        wire.now=voice.ag.codec_deadline;assert(pump()<0);puts("PASS mismatched BCS cannot start synchronous setup");return 0;}
    if(msbc_refuse){for(unsigned n=0;n<2000&&!atomic_load(&app.failed);++n)if(pump()<0)break;
        assert(atomic_load(&app.failed)&&msbc_setups==1&&!atomic_load(&voice.segment_disconnected));puts("PASS command refusal retains without speculative fallback");return 0;}
    until(CAPTURE);assert(stream()->codec==(msbc_no_cap||msbc_fallback?1u:2u));
    if(!strcmp(argv[1],"cadence")){
        /* mSBC silence follows the 7.5 ms eSCO frame period on a jittered poll grid. */
        uint64_t start=wire.now,before=sco_bytes;jittered(900000);
        uint64_t frames=(sco_bytes-before)/60,expected=(wire.now-start)/7500;
        fprintf(stderr,"mSBC silence %llu frames over %llu us, expected %llu\n",(unsigned long long)frames,(unsigned long long)(wire.now-start),(unsigned long long)expected);
        assert(frames<=expected+1&&frames+1>=expected&&!voice.silence_restarts);
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
    if(msbc_no_cap||msbc_fallback){uint8_t pcm[240]={0};inject(pcm,sizeof(pcm),0);for(unsigned n=0;n<20&&!atomic_load(&voice.segment_head);++n)assert(pump()>=0);take(1,NULL);close_audio();puts("PASS explicit CVSD fallback identity");return 0;}
    sbc_t encoder;assert(!sbc_init_msbc(&encoder,0));encoder.endian=SBC_LE;
    uint8_t input[96000]={0};unsigned total=speech?sizeof(input):!strcmp(argv[1],"backlog")?720:240;
    FILE *out=NULL;if(speech){assert(argc==4);FILE *in=fopen(argv[2],"rb");out=fopen(argv[3],"wb");assert(in&&out&&fread(input,1,sizeof(input),in)==sizeof(input)&&fgetc(in)==EOF&&!fclose(in));}
    unsigned seq=0,taken=0;struct bluetooth_segment_snapshot snapshots[2];
    for(unsigned off=0;off<total;off+=240){uint8_t frame[60]={1};ssize_t written=0;const uint8_t h2[]={8,0x38,0xc8,0xf8};frame[1]=h2[seq++&3];
        assert(sbc_encode(&encoder,input+off,240,frame+2,57,&written)==240&&written==57);
        if(!strcmp(argv[1],"crc"))frame[5]^=1;
        if(!strcmp(argv[1],"sequence")){if(off==0){frame[1]=0x38;inject(frame,60,0);for(unsigned n=0;n<6;++n)assert(pump()>=0);take(1,NULL);}frame[1]=0x08;}
        if(!strcmp(argv[1],"loss")){inject(frame,60,2);}else {
            /* Initial alignment and every header/frame are split across HCI records. */
            if(!off&&strcmp(argv[1],"sequence")){const uint8_t prefix[]={0x74,0x22,0x00};inject(prefix,3,0);for(unsigned n=0;n<4;++n)assert(pump()>=0);}
            for(unsigned at=0;at<60;){unsigned n=(at%7)+1;if(n>60-at)n=60-at;inject(frame+at,n,0);for(unsigned k=0;k<4;++k)assert(pump()>=0);at+=n;}
        }
        if(!strcmp(argv[1],"crc")||!strcmp(argv[1],"loss")||!strcmp(argv[1],"sequence")){
            until(CLOSE);assert(atomic_load(&voice.segment_flags)&BLUETOOTH_SEGMENT_LOSS);assert(atomic_load(&voice.segment_head)==(!strcmp(argv[1],"sequence")?1u:0u));close_audio();sbc_finish(&encoder);puts("PASS codec/sequence/HCI loss stops without publishing damaged PCM");return 0;}
        if(strcmp(argv[1],"backlog")&&atomic_load(&voice.segment_head)>taken){assert(taken<2);snapshots[taken]=take(taken+1,out);++taken;}
    }
    if(!strcmp(argv[1],"backlog")){until(CLOSE);assert(atomic_load(&voice.segment_head)==2&&(atomic_load(&voice.segment_flags)&BLUETOOTH_SEGMENT_BACKLOG));take(1,NULL);take(2,NULL);close_audio();sbc_finish(&encoder);puts("PASS full decoded ring retains accepted PCM and stops before overwrite");return 0;}
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
