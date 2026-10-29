/* Production UART/H4/HCI/security/AG/SCO and repeated voice-recognition
 * segments: one segment per activation, borrowed by the bound consumer. */
#define BLUETOOTH_CHECK_REPEATED 1
#define BLUETOOTH_HEADSET_FIXTURE_ENTRY archived_headset_main
#include "bluetooth-headset.c"
#undef BLUETOOTH_HEADSET_FIXTURE_ENTRY
static struct bluetooth_segment_snapshot taken_snapshots[2];
static const char *latency_codec="CVSD";
static TaskHandle_t segment_reader=(TaskHandle_t)2;
static void inject_pcm(const uint8_t *p,unsigned length,unsigned status)
{uint8_t h[]={3,0,(uint8_t)(status<<4),(uint8_t)length};assert(length<=255);bytes(&wire,h,4);bytes(&wire,p,length);}
static void pumps(unsigned n){for(unsigned i=0;i<n;++i)assert(pump()>=0);}
static void hf(const char *at){rf_in(14,0xef,(const uint8_t *)at,(unsigned)strlen(at),-1);}
/* The consumer's request with the head it last observed. */
static int request(unsigned word)
{current_task=segment_reader;int r=bluetooth_headset_segment_request(&app,word,atomic_load(&voice.segment_head));current_task=owner;return r;}
static unsigned word(void){return atomic_load(&voice.segment_request);}
static uint64_t head(void){return atomic_load(&voice.segment_head);}
static void idle_closed(void);
/* Service-level connection with the HF's BRSF features, ending idle: no audio
 * link exists before the first activation. */
static void connected(unsigned features)
{
    until(CONNECT);const uint8_t request[]={1,2,3,4,5,6,4,4,0x24,1};event(&wire,4,request,sizeof(request));
    until(CONTROL);const uint8_t connect[]={3,0,0x61,0};sig_in(2,50,connect,4);until(AG);
    char at[80];snprintf(at,sizeof(at),"AT+BRSF=%u\rAT+CIND=?\rAT+CIND?\rAT+CMER=3,,,1\r",features);hf(at);until(IDLE);
    assert(voice.voice&&brsf==BT_HFP_AG_VOICE+call_features&&!voice.stream&&!sco_setups&&!voice.operation_deadline);
}
/* Runs the owner until the condition holds, within a bounded number of polls. */
#define UNTIL(condition) do{for(unsigned n_=0;n_<400&&!(condition);++n_)assert(pump()>=0);assert(condition);}while(0)
/* Borrows segment `ticket`, checks it against the expected bytes and the
 * owner's digest, and acknowledges it. */
static void take(uint64_t ticket,const uint8_t *expected,size_t length,uint64_t first_sample,FILE *out)
{
    const uint8_t *pcm=NULL;struct bluetooth_segment_snapshot s;
    assert(bluetooth_headset_segment_take(&app,&pcm,&s)==-1);
    current_task=segment_reader;assert(bluetooth_headset_segment_take(&app,&pcm,&s)==1);
    assert(s.ticket==ticket&&s.first_sample==first_sample&&s.capture.bytes==length&&s.capture.state==1&&
        pcm==voice.segments[(ticket-1)%BLUETOOTH_SEGMENT_SLOTS].pcm&&s.capture.firmware==app.firmware_generation&&
        s.capture.uart==app.uart_generation&&s.capture.acl==voice.link&&s.capture.audio&&s.capture.audio<=voice.stream&&!s.capture.loss_packets);
    if(expected)assert(!memcmp(pcm,expected,length));
    uint8_t digest[32];storage_digest_bytes(pcm,length,digest);assert(!memcmp(digest,s.capture.sha256,32));
    if(out)assert(fwrite(pcm,1,length,out)==length);
    if(ticket<=2)taken_snapshots[ticket-1]=s;
    assert(bluetooth_headset_segment_ack(&app,ticket+1)==-1);
    assert(bluetooth_headset_segment_ack(&app,ticket)==1&&bluetooth_headset_segment_ack(&app,ticket)==-1);current_task=owner;
}
/* With the link closed, the polls from a commit to the synchronous-connection
 * setup accepted by the UART; the link then opens. */
static void link_setup(const char *name)
{
    if(voice.stage==CAPTURE)return;
    unsigned setups=sco_setups;LATENCY(name,POLLS(sco_setups==setups+1),4);
    UNTIL(voice.stage==CAPTURE);
}
/* One owner-started activation: +BVRA: 1, then the open link. */
static void started(void)
{
    if(voice.stage!=IDLE&&voice.idle_close)idle_closed();
    unsigned on=bvra_on;assert(request(BLUETOOTH_SEGMENT_START)==1&&word()==BLUETOOTH_SEGMENT_START);
    LATENCY("START to committed on",POLLS(voice.ag.recognition),5);assert(voice.active);
    link_setup("START commit to setup accepted");
    UNTIL(voice.active&&voice.stage==CAPTURE);assert(bvra_on==on+1&&voice.bvra&&voice.ag.recognition&&word()==BLUETOOTH_SEGMENT_ACTIVE);
}
/* The activation ended by a full segment: +BVRA: 0 follows publication. */
static void ended(uint64_t published)
{
    unsigned off=bvra_off;UNTIL(head()==published);assert(!voice.active&&!word());
    UNTIL(bvra_off==off+1);assert(!voice.ag.recognition&&!voice.voice_off);
}
/* The polls from a packet's arrival to its first byte in a slot, and, when
 * the packet fills the segment, from publication to +BVRA: 0 queued. */
static void first_pcm(const uint8_t *pcm,unsigned length)
{
    uint64_t published=head();unsigned off=bvra_off;inject_pcm(pcm,length,0);
    LATENCY("packet to first PCM byte in a slot",POLLS(voice.segment_bytes||head()!=published),1);
    if(head()!=published)LATENCY("publication to +BVRA: 0 queued",POLLS(voice.ag.response_length||bvra_off!=off),1);
}
static void disconnect(void)
{
    for(unsigned n=0;n<8;++n)assert(pump()>=0);
    assert(!atomic_load(&voice.segment_disconnected)&&voice.audio.links[0].phase==BT_SCO_DISCONNECT_WAIT);
    const uint8_t done[]={0,0,0,0x13};event(&wire,5,done,sizeof(done));until(DONE);
    assert(atomic_load(&voice.segment_disconnected)&&!word());
}
static void closed(void){assert(bluetooth_headset_segment_stop(&app)==1);if(voice.stage==IDLE){until(DONE);return;}until(CLOSE);disconnect();}
/* The headset button: AT+BVRA=1 is refused until the consumer admits it and
 * AT+BVRA=0 until recognition is on. Odd packets released between
 * activations keep the next segment aligned to whole samples. */
static void button(const uint8_t *pcm)
{
    connected(BT_HFP_AG_HF_VOICE);
    hf("AT+BVRA=1\r");pumps(20);assert(at_error==1&&!voice.active&&!voice.ag.recognition&&!sco_setups);
    hf("AT+BVRA=0\r");pumps(20);assert(at_error==2);
    assert(request(BLUETOOTH_SEGMENT_ACCEPT)==1&&request(BLUETOOTH_SEGMENT_ACCEPT)==1&&word()==BLUETOOTH_SEGMENT_ACCEPT);
    assert(request(0)==-1&&request(BLUETOOTH_SEGMENT_ACTIVE)==-1&&bluetooth_headset_segment_request(&app,BLUETOOTH_SEGMENT_START,0)==-1);
    unsigned ok=at_ok;hf("AT+BVRA=1\r");LATENCY("AT+BVRA=1 line to committed on",POLLS(voice.ag.recognition),4);
    link_setup("AT+BVRA=1 commit to setup accepted");
    assert(at_ok==ok+1&&voice.active&&voice.bvra&&voice.ag.recognition&&word()==BLUETOOTH_SEGMENT_ACTIVE&&!bvra_on&&sco_setups==1);
    /* A 13-byte packet fills the 8-byte segment; its tail is released. */
    unsigned woken=notifications;first_pcm(pcm,13);ended(1);
    /* A request that has not seen the new segment is refused. */
    current_task=segment_reader;assert(bluetooth_headset_segment_request(&app,BLUETOOTH_SEGMENT_ACCEPT,0)==0&&!word());current_task=owner;
    assert(notifications==woken+1&&(voice.position&1)&&!voice.segment_bytes&&!voice.partial_serial);
    take(1,pcm,8,0,NULL);
    /* The next connection starts at its own sample boundary. */
    idle_closed();assert(request(BLUETOOTH_SEGMENT_ACCEPT)==1);hf("AT+BVRA=1\r");UNTIL(voice.active&&voice.stage==CAPTURE);
    assert(!voice.skip&&sco_setups==2);
    const uint8_t next[]={21,22,23,24,25,26,27,28};inject_pcm(next,8,0);ended(2);
    take(2,next,8,4,NULL);
    assert(bvra_off==2&&!bvra_on&&notifications==woken+2);
}
/* With only ACCEPT armed and no link, the headset closes HFP: DISC on its
 * DLC and on DLCI 0, then an L2CAP disconnection request, then the ACL
 * disconnection. The AG leaves the ready state, which the owner never
 * requests, so the session fails with retention at once. */
static void idle_disconnect(void)
{
    assert(request(BLUETOOTH_SEGMENT_ACCEPT)==1);pumps(5);assert(voice.stage==IDLE&&voice.ag.phase==BT_HFP_AG_READY);
    rf_in(14,0x53,NULL,0,-1);int r=0;unsigned n=0;
    for(;n<5&&r>=0;++n)r=pump();
    if(r>=0){rf_in(0,0x53,NULL,0,-1);for(n=0;n<5&&r>=0;++n)r=pump();}
    if(r>=0){uint8_t disc[4];put16(disc,local_cid);put16(disc+2,0x61);sig_in(6,77,disc,4);for(n=0;n<20&&r>=0;++n)r=pump();}
    if(r>=0){const uint8_t lost[]={0,0x23,0,0x13};event(&wire,5,lost,sizeof(lost));for(n=0;n<20&&r>=0;++n)r=pump();}
    assert(r==-1&&(atomic_load(&recovery)&4)&&!atomic_load(&voice.segment_disconnected)&&voice.stage==IDLE&&!sco_setups);
    fprintf(stderr,"idle disconnect: AG phase %u, RFCOMM phase %u\n",(unsigned)voice.ag.phase,(unsigned)voice.rfcomm.phase);
}
/* The peer confirms the local disconnect; all copied records retire before
 * idle. The controller deliberately reuses handle zero on the next setup. */
static void idle_closed(void)
{
    UNTIL(voice.stage==CLOSE&&stream()->phase==BT_SCO_DISCONNECT_WAIT);
    assert(!voice.ag.recognition&&!voice.ag.response_length&&!atomic_load(&voice.segment_disconnected));
    const uint8_t done[]={0,0,0,0x13};event(&wire,5,done,sizeof(done));until(IDLE);
    assert(!atomic_load(&voice.segment_disconnected)&&stream()->phase==BT_SCO_CLOSED);
}
static void idle_cases(const char *name,const uint8_t *pcm)
{
    connected(BT_HFP_AG_HF_VOICE);started();uint64_t first=voice.stream;
    first_pcm(pcm,13);ended(1);take(1,pcm,8,0,NULL);
    if(!strcmp(name,"idle-traffic")){
        /* The preserved baseline has no idle close. This measurement runs
         * against either source without fabricating a disconnect there. */
        pumps(12);if(voice.stage==CLOSE)idle_closed();
        uint64_t before=sco_bytes,at=wire.now;jittered(900000);
        fprintf(stderr,"IDLE CVSD bytes=%llu elapsed_us=%llu stage=%u\n",
            (unsigned long long)(sco_bytes-before),(unsigned long long)(wire.now-at),voice.stage);
        if(voice.stage==IDLE)assert(sco_bytes==before);
        else assert(voice.stage==CAPTURE&&sco_bytes>before);
        return;
    }
    UNTIL(voice.stage==CLOSE&&stream()->phase==BT_SCO_DISCONNECT_WAIT);
    assert(!voice.ag.recognition&&!voice.ag.response_length);
    if(!strcmp(name,"idle-close-timeout")){
        wire.now=voice.operation_deadline;assert(pump()<0&&(atomic_load(&recovery)&4));
        assert(stream()->phase==BT_SCO_DISCONNECT_WAIT&&!atomic_load(&voice.segment_disconnected));return;
    }
    if(!strcmp(name,"idle-stop-close")){
        assert(request(BLUETOOTH_SEGMENT_START)==1&&bluetooth_headset_segment_stop(&app)==1);
        disconnect();assert(sco_setups==1&&!voice.active);return;
    }
    if(!strcmp(name,"idle-start")){
        assert(request(BLUETOOTH_SEGMENT_START)==1);hf("AT+BVRA=1\r");unsigned errors=at_error;
        pumps(20);assert(at_error==errors+1&&!voice.active&&sco_setups==1&&word()==BLUETOOTH_SEGMENT_START);
        idle_closed();LATENCY("pending START after idle close to new capture",POLLS(voice.stage==CAPTURE),16);
    }else{
        idle_closed();uint64_t before=sco_bytes;jittered(900000);assert(sco_bytes==before);
        fprintf(stderr,"IDLE CVSD bytes=0 over >=900000 us\n");
        if(!strcmp(name,"idle-stop-open")){
            assert(request(BLUETOOTH_SEGMENT_START)==1);UNTIL(voice.stage==AUDIO);
            assert(bluetooth_headset_segment_stop(&app)==1);until(CLOSE);disconnect();
            assert(sco_setups==2&&head()==1);return;
        }
        started();
    }
    assert(voice.stream==first+1&&sco_setups==2&&!voice.skip&&!voice.position&&!stream()->handle);
    first_pcm(pcm+8,8);ended(2);take(2,pcm+8,8,4,NULL);
    assert(taken_snapshots[0].capture.audio==first&&taken_snapshots[1].capture.audio==first+1);
    idle_closed();assert(bluetooth_headset_segment_stop(&app)==1);until(DONE);
    assert(atomic_load(&voice.segment_disconnected)&&sco_setups==2);
}
int main(int argc,char **argv)
{
    assert(argc==2||argc==4);speech=!strcmp(argv[1],"speech");
    resident_lifetime=!strcmp(argv[1],"resident")||!strcmp(argv[1],"peer-release")||!strcmp(argv[1],"idle-disconnect-resident");setup();
    scheduler=taskSCHEDULER_NOT_STARTED;assert(bluetooth_headset_segment_bind(&app,segment_reader)==1);scheduler=taskSCHEDULER_RUNNING;
    uint8_t pcm[17];for(unsigned n=0;n<17;++n)pcm[n]=(uint8_t)(n+1);
    if(!strncmp(argv[1],"idle-",5)&&strcmp(argv[1],"idle-stop")&&strncmp(argv[1],"idle-disconnect",15)){
        idle_cases(argv[1],pcm);puts("PASS idle audio close/reopen application case");return 0;
    }
    if(!strcmp(argv[1],"setup-stop")||!strcmp(argv[1],"ag-stop")){
        /* Before idle a stop retains the transports: commands may be in flight. */
        until(CONNECT);
        if(argv[1][0]=='a'){const uint8_t request[]={1,2,3,4,5,6,4,4,0x24,1};event(&wire,4,request,sizeof(request));
            until(CONTROL);const uint8_t connect[]={3,0,0x61,0};sig_in(2,50,connect,4);until(AG);}
        assert(bluetooth_headset_segment_stop(&app)==1&&pump()==-1&&(atomic_load(&recovery)&4)&&!atomic_load(&voice.segment_disconnected));
        puts("PASS a stop before the AG is ready retains the transports");return 0;
    }
    if(resident_lifetime){
        /* Resident: the firmware becomes ready at 100 s, which moves the
         * schedule origin to readiness. Activations continue past the supervised
         * 38 s window, one every 300 ms, and a stop closes within 2 s. */
        assert(pump()==0&&app.stage==1);wire.now=100000000;bootstrap_ready=1;
        connected(BT_HFP_AG_HF_VOICE);assert(app.started>=100000000&&app.started<100001000&&app.hci_deadline==app.started+5000000&&
            bluetooth_capture_window(&app)==UINT64_MAX);
        if(!strcmp(argv[1],"idle-disconnect-resident")){idle_disconnect();
            puts("PASS a headset that closes HFP while the resident owner is idle fails the session");return 0;}
        if(!strcmp(argv[1],"peer-release")){
            /* A headset that releases the link after +BVRA: 0 fails the session. */
            started();inject_pcm(pcm,8,0);ended(1);take(1,pcm,8,0,NULL);
            const uint8_t released[]={0,0,0,0x13};event(&wire,5,released,sizeof(released));
            int r=0;for(unsigned n=0;n<20&&r>=0;++n)r=pump();
            assert(r==-1&&(atomic_load(&recovery)&4)&&!atomic_load(&voice.segment_disconnected));
            puts("PASS an unrequested link release after +BVRA: 0 fails the resident session");return 0;
        }
        jittered(38000000);uint64_t stop=wire.now+1800000;
        for(uint64_t ticket=1;wire.now<stop;++ticket){
            started();inject_pcm(pcm,8,0);ended(ticket);take(ticket,pcm,8,(ticket-1)*4,NULL);idle_closed();jittered(300000);
            assert(head()==ticket&&!atomic_load(&voice.segment_flags)&&voice.stage==IDLE&&!voice.ag.recognition);
        }
        uint64_t stopped_at=wire.now;closed();assert(wire.now-stopped_at<2000000&&sco_setups==head());
        puts("PASS resident lifetime: origin at firmware readiness, activations past the supervised window, bounded close");return 0;
    }
    if(!strcmp(argv[1],"button")){
        button(pcm);idle_closed();
        /* A stop clears the admission, so no later stage accepts AT+BVRA=1. */
        assert(request(BLUETOOTH_SEGMENT_ACCEPT)==1);pumps(2);assert(voice.ag.voice_admit);
        closed();assert(!voice.ag.voice_admit);
        unsigned errors=at_error;hf("AT+BVRA=1\r");pumps(20);assert(at_error==errors+1&&!voice.ag.recognition);
        puts("PASS headset button activations, refusals, +BVRA: 0 and sample alignment");return 0;
    }
    if(!strcmp(argv[1],"no-voice")){
        /* Without the HF feature the button is refused and START opens audio
         * without +BVRA (HFP 4.11). */
        connected(0);assert(request(BLUETOOTH_SEGMENT_ACCEPT)==1);hf("AT+BVRA=1\r");pumps(20);assert(at_error==1&&!voice.active);
        assert(request(BLUETOOTH_SEGMENT_START)==1);UNTIL(voice.active&&voice.stage==CAPTURE);
        assert(!voice.bvra&&!bvra_on&&!voice.ag.recognition);inject_pcm(pcm,8,0);UNTIL(head()==1);
        pumps(20);assert(!voice.active&&!word()&&!bvra_off&&!voice.voice_off);take(1,pcm,8,0,NULL);closed();
        puts("PASS a headset without voice recognition records on START without +BVRA");return 0;
    }
    if(!strcmp(argv[1],"idle-disconnect")){
        connected(BT_HFP_AG_HF_VOICE);idle_disconnect();
        puts("PASS a headset that closes HFP while the owner is idle fails the session");return 0;
    }
    if(!strcmp(argv[1],"window")){
        /* The supervised window ends idle cleanly, without retention. */
        connected(BT_HFP_AG_HF_VOICE);assert(request(BLUETOOTH_SEGMENT_ACCEPT)==1);
        jittered(app.started+38000000-wire.now);UNTIL(voice.stage==DONE);
        assert(atomic_load(&voice.segment_flags)==BLUETOOTH_SEGMENT_WINDOW&&atomic_load(&voice.segment_disconnected)&&
            !atomic_load(&app.failed)&&!sco_setups&&!word());pumps(20);
        puts("PASS the capture window ends idle without opening audio");return 0;
    }
    if(!strcmp(argv[1],"idle-stop")){
        /* A stop while +BVRA: 1 is in flight waits for the response, then ends
         * idle without an activation or retention. */
        connected(BT_HFP_AG_HF_VOICE);UNTIL(!voice.ag.response_length);
        assert(request(BLUETOOTH_SEGMENT_START)==1);UNTIL(voice.ag.response_length);assert(voice.ag.pending_recognition);
        /* Without UART room the response stays in flight. */
        assert(bluetooth_headset_segment_stop(&app)==1);
        for(unsigned n=0;n<3;++n){wire.room=0;++wire.now;assert(bluetooth_application_poll(&app)>=0);}
        assert(voice.stage==IDLE&&voice.ag.response_length&&voice.operation_deadline==app.started+40000000);
        UNTIL(voice.stage==DONE);
        assert(!voice.ag.response_length&&!voice.active&&!sco_setups&&atomic_load(&voice.segment_disconnected)&&
            atomic_load(&voice.segment_flags)==BLUETOOTH_SEGMENT_CANCEL&&!atomic_load(&app.failed)&&!voice.operation_deadline);
        pumps(20);puts("PASS a stop with an AG response in flight ends idle after the response");return 0;
    }
    if(!strcmp(argv[1],"open-stop")){
        /* A stop while the link opens waits for it, then closes it. */
        connected(BT_HFP_AG_HF_VOICE);assert(request(BLUETOOTH_SEGMENT_ACCEPT)==1);hf("AT+BVRA=1\r");UNTIL(voice.stage==AUDIO);
        assert(bluetooth_headset_segment_stop(&app)==1);until(CLOSE);assert(!voice.ag.recognition&&bvra_off==1);disconnect();
        assert(!head()&&atomic_load(&voice.segment_flags)==BLUETOOTH_SEGMENT_CANCEL&&!atomic_load(&app.failed));
        /* The stop turned the headset's recognition off before DONE. */
        assert(bvra_off==1&&!voice.ag.recognition&&!voice.ag.response_length);
        puts("PASS a stop during the link open closes the opened link and sends +BVRA: 0");return 0;
    }
    if(!strcmp(argv[1],"early")){
        /* AT+BVRA=0 ends an activation early: an odd byte count publishes
         * its even prefix, and one byte or none publishes nothing. */
        connected(BT_HFP_AG_HF_VOICE);assert(request(BLUETOOTH_SEGMENT_ACCEPT)==1);hf("AT+BVRA=1\r");UNTIL(voice.active&&voice.stage==CAPTURE);
        inject_pcm(pcm,5,0);UNTIL(voice.segment_bytes==5);hf("AT+BVRA=0\r");
        LATENCY("AT+BVRA=0 line to publication",POLLS(head()==1),4);
        assert(!voice.active&&!word()&&!voice.voice_off&&!bvra_off);take(1,pcm,4,0,NULL);
        unsigned woken=notifications;idle_closed();
        assert(request(BLUETOOTH_SEGMENT_ACCEPT)==1);hf("AT+BVRA=1\r");UNTIL(voice.active&&voice.stage==CAPTURE);assert(!voice.skip);
        const uint8_t split[]={7};inject_pcm(split,1,0);UNTIL(voice.segment_bytes==1);hf("AT+BVRA=0\r");UNTIL(!voice.active);
        assert(head()==1&&!word()&&notifications==woken+1);idle_closed();
        assert(request(BLUETOOTH_SEGMENT_ACCEPT)==1);hf("AT+BVRA=1\r");UNTIL(voice.active&&voice.stage==CAPTURE);hf("AT+BVRA=0\r");UNTIL(!voice.active);
        assert(head()==1&&!word()&&notifications==woken+2);idle_closed();
        assert(request(BLUETOOTH_SEGMENT_ACCEPT)==1);hf("AT+BVRA=1\r");UNTIL(voice.active);
        UNTIL(voice.stage==CAPTURE);const uint8_t next[]={31,32,33,34,35,36,37,38};inject_pcm(next,8,0);UNTIL(head()==2);
        take(2,next,8,2,NULL);closed();assert(!bvra_on);
        puts("PASS early ends publish the even prefix; empty activations publish nothing");return 0;
    }
    if(!strcmp(argv[1],"pending-off")){
        /* A request between publication and acceptance of +BVRA: 0 waits for
         * it: no activation starts while recognition is still on. */
        connected(BT_HFP_AG_HF_VOICE);started();inject_pcm(pcm,8,0);UNTIL(head()==1);
        assert(voice.voice_off&&voice.ag.recognition&&!bvra_off);assert(request(BLUETOOTH_SEGMENT_START)==1);
        UNTIL(bvra_off==1);assert(!voice.active);idle_closed();UNTIL(bvra_on==2&&voice.active&&voice.stage==CAPTURE);assert(voice.ag.recognition&&word()==BLUETOOTH_SEGMENT_ACTIVE);
        inject_pcm(pcm+8,8,0);ended(2);take(1,pcm,8,0,NULL);take(2,pcm+8,8,4,NULL);closed();
        puts("PASS a request before +BVRA: 0 is accepted starts only after it");return 0;
    }
    if(!strcmp(argv[1],"start")){
        /* Owner-started activations on successive links: +BVRA: 1 and +BVRA: 0 each. */
        connected(BT_HFP_AG_HF_VOICE);started();first_pcm(pcm,8);ended(1);take(1,pcm,8,0,NULL);
        started();first_pcm(pcm+8,8);ended(2);take(2,pcm+8,8,4,NULL);
        assert(bvra_on==2&&bvra_off==2&&sco_setups==2);closed();puts("PASS owner-started activations exchange both +BVRA responses");return 0;
    }
    connected(BT_HFP_AG_HF_VOICE);
    /* A stop stored after the poll's first check, while the owner publishes
     * the first segment, still closes the stream within the close bound. */
    if(!strcmp(argv[1],"stop-in-poll")){
        started();
        for(unsigned n=0;n<50;++n)assert(pump()>=0);
        assert(voice.audio.active_tx==BT_SCO_PACKETS&&app.hci.transport.tx_offset==app.hci.transport.tx_length&&wire.now+100<voice.next_silence);
        stop_at_publication=1;inject_pcm(pcm,8,0);
        for(unsigned n=0;n<40&&voice.stage==CAPTURE;++n)assert(pump()>=0);
        assert(!stop_at_publication&&voice.stage==CLOSE&&head()==1&&
            atomic_load(&voice.segment_flags)==BLUETOOTH_SEGMENT_CANCEL&&voice.operation_deadline==app.started+40000000);
        disconnect();puts("PASS stop stored during a capture poll closes cleanly");return 0;
    }
    if(speech){
        assert(argc==4);FILE *in=fopen(argv[2],"rb"),*out=fopen(argv[3],"wb");assert(in&&out);
        uint8_t input[96000];assert(fread(input,1,sizeof(input),in)==sizeof(input)&&fgetc(in)==EOF&&!fclose(in));
        for(unsigned segment=0;segment<2;++segment){
            started();
            for(unsigned off=0;off<48000;){unsigned count=48000-off;if(count>255)count=255;
                inject_pcm(input+segment*48000+off,count,0);for(unsigned n=0;n<5;++n)assert(pump()>=0);off+=count;}
            ended(segment+1);take(segment+1,input+segment*48000,48000,segment*24000,out);
        }
        assert(!fclose(out)&&atomic_load(&voice.segment_tail)==2);
        char name[4096];assert(snprintf(name,sizeof(name),"%s.snapshots.bin",argv[3])>0);
        FILE *meta=fopen(name,"wb");assert(meta&&fwrite(taken_snapshots,1,sizeof(taken_snapshots),meta)==sizeof(taken_snapshots)&&!fclose(meta));closed();puts("PASS two actual-stack speech segments, one per activation, on successive audio connections");return 0;
    }
    started();
    if(!strcmp(argv[1],"loss")){
        inject_pcm(pcm,8,1);until(CLOSE);assert(!head()&&(atomic_load(&voice.segment_flags)&BLUETOOTH_SEGMENT_LOSS));
        disconnect();assert(bvra_off==1&&!voice.ag.recognition);
        puts("PASS loss stops capture without publishing and sends +BVRA: 0");return 0;
    }
    if(!strcmp(argv[1],"backlog")){
        /* Two unacknowledged segments fill both slots; a third activation
         * stops at its first write instead of overwriting a borrowed slot. */
        inject_pcm(pcm,8,0);ended(1);started();inject_pcm(pcm+8,8,0);ended(2);started();inject_pcm(pcm,8,0);
        until(CLOSE);assert(head()==2&&(atomic_load(&voice.segment_flags)&BLUETOOTH_SEGMENT_BACKLOG));
        take(1,pcm,8,0,NULL);take(2,pcm+8,8,4,NULL);disconnect();puts("PASS full slots stop capture before overwrite");return 0;
    }
    inject_pcm(pcm,8,0);ended(1);take(1,pcm,8,0,NULL);
    if(!strcmp(argv[1],"late-disconnect")){
        assert(bluetooth_headset_segment_stop(&app)==1);until(CLOSE);wire.now=40000000;
        assert(pump()==-1&&!atomic_load(&voice.segment_disconnected));puts("PASS late disconnect retains without stopped claim");return 0;
    }
    closed();assert(head()==1);
    if(!strcmp(argv[1],"revocation")){
        const uint8_t revoked[]={0,0x23,0,0};event(&wire,8,revoked,sizeof(revoked));
        for(unsigned n=0;n<20&&!atomic_load(&app.failed);++n)if(pump()<0)break;
        assert(atomic_load(&app.failed));struct bluetooth_segment_status status;
        assert(bluetooth_headset_segment_status(&app,&status)==-1);puts("PASS post-disconnect security revocation still serviced and denies publication");return 0;
    }
    puts("PASS one activation, exact borrow and ACK, and actual disconnect");return 0;
}
