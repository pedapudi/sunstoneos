/* Production UART/H4/HCI/security/AG/SCO and repeated copied-input path. */
#define BLUETOOTH_CHECK_REPEATED 1
#define BLUETOOTH_HEADSET_FIXTURE_ENTRY archived_headset_main
#include "bluetooth-headset.c"
#undef BLUETOOTH_HEADSET_FIXTURE_ENTRY
static struct bluetooth_segment_snapshot taken_snapshots[2];
static TaskHandle_t segment_reader=(TaskHandle_t)2;
static void inject_pcm(const uint8_t *p,unsigned length,unsigned status)
{uint8_t h[]={3,0,(uint8_t)(status<<4),(uint8_t)length};assert(length<=255);bytes(&wire,h,4);bytes(&wire,p,length);}
static void connected(void)
{
    until(CONNECT);const uint8_t request[]={1,2,3,4,5,6,4,4,0x24,1};event(&wire,4,request,sizeof(request));
    until(CONTROL);const uint8_t connect[]={3,0,0x61,0};sig_in(2,50,connect,4);until(AG);
    const char *at="AT+BRSF=0\rAT+CIND=?\rAT+CIND?\rAT+CMER=3,,,1\r";
    rf_in(14,0xef,(const uint8_t *)at,(unsigned)strlen(at),-1);until(CAPTURE);
}
static void take(uint64_t ticket,const uint8_t *expected,size_t length,FILE *out)
{
    current_task=segment_reader;uint8_t data[1024];size_t copied;struct bluetooth_segment_snapshot snapshot,identity={0};
    for(size_t off=0;off<length;){size_t n=length-off;if(n>1024)n=1024;
        assert(bluetooth_headset_segment_copy(&app,ticket,(uint32_t)off,n,data,&copied,&snapshot)==1&&copied==n);
        if(!off)identity=snapshot;else assert(!memcmp(&identity,&snapshot,sizeof(identity)));
        if(expected)assert(!memcmp(data,expected+off,n));if(out)assert(fwrite(data,1,n,out)==n);off+=n;}
    assert(identity.ticket==ticket&&identity.first_sample==(ticket-1)*profile.headset.samples&&identity.capture.bytes==length&&
        identity.capture.firmware==app.firmware_generation&&identity.capture.uart==app.uart_generation&&!identity.capture.loss_packets);
    if(ticket<=2)taken_snapshots[ticket-1]=identity;
    assert(bluetooth_headset_segment_ack(&app,ticket+1)==-1);
    assert(bluetooth_headset_segment_ack(&app,ticket)==1&&bluetooth_headset_segment_ack(&app,ticket)==-1);current_task=owner;
}
static void closed(void)
{
    assert(bluetooth_headset_segment_stop(&app)==1);until(CLOSE);
    for(unsigned n=0;n<8;++n)assert(pump()>=0);
    assert(!atomic_load(&voice.segment_disconnected)&&voice.audio.links[0].phase==BT_SCO_DISCONNECT_WAIT);
    const uint8_t done[]={0,0,0,0x13};event(&wire,5,done,sizeof(done));until(DONE);
    assert(atomic_load(&voice.segment_disconnected));
}
/* Resident lifetime: the firmware becomes ready at 100 s, which moves the
 * schedule origin to 70 s. Capture then runs past the supervised 38 s window
 * with one segment every 300 ms, and a stop closes within 2 s of itself. */
static void resident(const uint8_t *pcm)
{
    assert(pump()==0&&app.stage==1);wire.now=100000000;bootstrap_ready=1;
    connected();assert(app.started>=70000000&&app.started<70001000&&app.hci_deadline==app.started+35000000&&
        bluetooth_capture_window(&app)==UINT64_MAX);
    uint64_t stop=app.started+38000000+5000000;
    for(uint64_t ticket=1;wire.now<stop;++ticket){
        inject_pcm(pcm,8,0);jittered(300000);
        assert(atomic_load(&voice.segment_head)==ticket&&!atomic_load(&voice.segment_flags)&&voice.stage==CAPTURE);
        take(ticket,pcm,8,NULL);
    }
    uint64_t stopped_at=wire.now;assert(bluetooth_headset_segment_stop(&app)==1);until(CLOSE);
    assert(voice.operation_deadline>stopped_at&&voice.operation_deadline<=stopped_at+2000000+10);
}
int main(int argc,char **argv)
{
    assert(argc==2||argc==4);speech=!strcmp(argv[1],"speech");resident_lifetime=!strcmp(argv[1],"resident");setup();
    scheduler=taskSCHEDULER_NOT_STARTED;assert(bluetooth_headset_segment_bind(&app,segment_reader)==1);scheduler=taskSCHEDULER_RUNNING;
    uint8_t pcm[17];for(unsigned n=0;n<17;++n)pcm[n]=(uint8_t)(n+1);
    if(resident_lifetime){
        bootstrap_ready=0;resident(pcm);
        for(unsigned n=0;n<8;++n)assert(pump()>=0);
        const uint8_t done[]={0,0,0,0x13};event(&wire,5,done,sizeof(done));until(DONE);
        assert(atomic_load(&voice.segment_disconnected));
        puts("PASS resident lifetime: origin at firmware readiness, capture past the supervised window, bounded close");return 0;
    }
    connected();
    if(speech){
        assert(argc==4);FILE *in=fopen(argv[2],"rb"),*out=fopen(argv[3],"wb");assert(in&&out);
        uint8_t input[96000];assert(fread(input,1,sizeof(input),in)==sizeof(input)&&fgetc(in)==EOF&&!fclose(in));
        uint64_t taken=0;
        for(unsigned off=0;off<sizeof(input);){unsigned count=sizeof(input)-off;if(count>255)count=255;
            inject_pcm(input+off,count,0);for(unsigned n=0;n<5;++n)assert(pump()>=0);off+=count;
            if(atomic_load(&voice.segment_head)>taken){take(taken+1,input+taken*48000,48000,out);++taken;}}
        for(unsigned n=0;n<20&&atomic_load(&voice.segment_head)<2;++n)assert(pump()>=0);
        if(taken<2)take(2,input+48000,48000,out);
        assert(!fclose(out)&&atomic_load(&voice.segment_tail)==2);
        char name[4096];assert(snprintf(name,sizeof(name),"%s.snapshots.bin",argv[3])>0);
        FILE *meta=fopen(name,"wb");assert(meta&&fwrite(taken_snapshots,1,sizeof(taken_snapshots),meta)==sizeof(taken_snapshots)&&!fclose(meta));closed();puts("PASS two actual-stack copied speech segments on one audio connection");return 0;
    }
    inject_pcm(pcm,!strcmp(argv[1],"backlog")?17:13,!strcmp(argv[1],"loss"));
    for(unsigned n=0;n<40&&!atomic_load(&voice.segment_head)&&voice.stage==CAPTURE;++n)assert(pump()>=0);
    if(!strcmp(argv[1],"loss")){
        until(CLOSE);assert(!atomic_load(&voice.segment_head)&&(atomic_load(&voice.segment_flags)&BLUETOOTH_SEGMENT_LOSS));
    }else if(!strcmp(argv[1],"backlog")){
        until(CLOSE);assert(atomic_load(&voice.segment_head)==2&&(atomic_load(&voice.segment_flags)&BLUETOOTH_SEGMENT_BACKLOG));
        take(1,pcm,8,NULL);take(2,pcm+8,8,NULL);
    }else {
        assert(atomic_load(&voice.segment_head)==1&&voice.segment_bytes==5&&!voice.partial_serial);
        assert(voice.segments[1].pcm[0]==9&&voice.segments[1].pcm[4]==13);
        take(1,pcm,8,NULL);
        if(!strcmp(argv[1],"cancel")){assert(bluetooth_headset_segment_stop(&app)==1);until(CLOSE);assert(atomic_load(&voice.segment_head)==1);}
        else {
            inject_pcm(pcm+13,3,0);for(unsigned n=0;n<40&&atomic_load(&voice.segment_head)<2;++n)assert(pump()>=0);
            take(2,pcm+8,8,NULL);
        }
    }
    if(!strcmp(argv[1],"late-disconnect")){
        assert(bluetooth_headset_segment_stop(&app)==1);until(CLOSE);wire.now=40000000;
        assert(pump()==-1&&!atomic_load(&voice.segment_disconnected));puts("PASS late disconnect retains without stopped claim");return 0;
    }
    closed();
    if(!strcmp(argv[1],"revocation")){
        const uint8_t revoked[]={0,0x23,0,0};event(&wire,8,revoked,sizeof(revoked));
        for(unsigned n=0;n<20&&!atomic_load(&app.failed);++n)if(pump()<0)break;
        assert(atomic_load(&app.failed));struct bluetooth_segment_status status;
        assert(bluetooth_headset_segment_status(&app,&status)==-1);puts("PASS post-disconnect security revocation still serviced and denies publication");return 0;
    }
    puts("PASS repeated copied segments, packet-tail preservation, exact input ACK and actual disconnect");return 0;
}
