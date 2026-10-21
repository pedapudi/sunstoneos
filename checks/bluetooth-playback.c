/* Actual application/codec/HCI composition, explicit modeled peer/UART effects. */
#include <stdint.h>
#include <stdlib.h>
static unsigned playback_fixture_mode=4,playback_hold_credits,publication_stop;
static void playback_observe(const uint8_t *,unsigned);
#define BLUETOOTH_CHECK_PLAYBACK 1
#define BLUETOOTH_MSBC_FIXTURE_ENTRY archived_msbc_main
#include "bluetooth-msbc.c"
#undef BLUETOOTH_MSBC_FIXTURE_ENTRY
#define live playback_live
#include "../apps/bluetooth/playback.c"
#undef live
static unsigned critical;
void vPortEnterCritical(void)
{
    if(publication_stop&&!critical){publication_stop=0;TaskHandle_t before=current_task;current_task=owner;bluetooth_playback_stop(&app);current_task=before;}
    ++critical;
}
void vPortExitCritical(void){assert(critical);--critical;}
static uint8_t outgoing[60000];static unsigned outgoing_bytes;
static void playback_observe(const uint8_t *p,unsigned n)
{assert(n<=sizeof(outgoing)-outgoing_bytes);memcpy(outgoing+outgoing_bytes,p,n);outgoing_bytes+=n;}
static uint8_t caller[4800];
static void upload(uint64_t ticket)
{
    current_task=reader;const uint8_t *p=caller+(ticket-1)*2400;uint8_t hash[32];storage_digest_bytes(p,2400,hash);
    assert(bluetooth_playback_write(&app,ticket,1,p,100)==-1);
    for(unsigned i=0;i<2400;){unsigned n=2400-i;if(n>1024)n=1024;assert(bluetooth_playback_write(&app,ticket,i,p+i,n)==1);i+=n;}
    assert(bluetooth_playback_commit(&app,ticket,hash)==1);
    assert(bluetooth_playback_write(&app,ticket,0,p,100)==-1);current_task=owner;
}
static void finish_close(void)
{
    until(CLOSE);for(unsigned n=0;n<8;++n)assert(pump()>=0);
    assert(!atomic_load(&voice.segment_disconnected));const uint8_t done[]={0,0,0,0x13};event(&wire,5,done,sizeof(done));until(DONE);
    for(unsigned n=0;n<32;++n)assert(pump()>=0);
}
int main(int argc,char **argv)
{
    assert(argc==2);playback_fixture_mode=!strcmp(argv[1],"pcm-duplex")?5:4;
    assert(bluetooth_msbc_init()==1);setup();
    for(unsigned i=0;i<2400;++i){caller[i]=(uint8_t)(i*7+11);caller[i+2400]=(uint8_t)(i*13+29);}
    if(!strcmp(argv[1],"pcm-early-cancel")){
        current_task=reader;assert(bluetooth_playback_end(&app,1)==1);current_task=owner;
        assert(pump()<0&&atomic_load(&recovery)&4&&!atomic_load(&voice.segment_disconnected)&&voice.stage==CONFIGURE);puts("PASS early CANCEL retains before provider/setup progress");return 0;}
    if(!strcmp(argv[1],"pcm-publication")){
        current_task=reader;assert(bluetooth_playback_write(&app,1,0,caller,1024)==1&&bluetooth_playback_write(&app,1,1024,caller+1024,1024)==1&&bluetooth_playback_write(&app,1,2048,caller+2048,352)==1);
        uint8_t hash[32];storage_digest_bytes(caller,2400,hash);publication_stop=1;assert(bluetooth_playback_commit(&app,1,hash)==-1&&!atomic_load(&playback.accepted)&&!atomic_load(&playback.slots[0].state));puts("PASS owner closure linearizes before COMMIT publication");return 0;}
    if(playback_fixture_mode==5){scheduler=taskSCHEDULER_NOT_STARTED;assert(bluetooth_headset_segment_bind(&app,reader)==1);scheduler=taskSCHEDULER_RUNNING;}
    connect_audio();until(CAPTURE);assert(stream()->codec==2);
    if(playback_fixture_mode==5){
        char path[4096];snprintf(path,sizeof(path),"%s.profile.bin",getenv("SUNSTONE_DUPLEX_PCM"));FILE *f=fopen(path,"wb");assert(f&&fwrite(&profile,1,profile.bytes,f)==profile.bytes&&!fclose(f));
        snprintf(path,sizeof(path),"%s.caller.bin",getenv("SUNSTONE_DUPLEX_PCM"));f=fopen(path,"wb");assert(f&&fwrite(caller,1,sizeof(caller),f)==sizeof(caller)&&!fclose(f));
        snprintf(path,sizeof(path),"%s.admission.json",getenv("SUNSTONE_DUPLEX_PCM"));f=fopen(path,"w");assert(f);
        fprintf(f,"{\"acl\":%llu,\"audio\":%llu,\"security\":%llu,\"capacity\":%u,\"mtu\":%u,\"handle\":%u,\"codec\":%u,\"firmware\":%llu,\"uart\":%llu}\n",(unsigned long long)voice.link,(unsigned long long)voice.stream,(unsigned long long)stream()->security.generation,voice.audio.capacity,voice.audio.mtu,stream()->handle,stream()->codec,(unsigned long long)app.firmware_generation,(unsigned long long)app.uart_generation);assert(!fclose(f));
    }
    if(!strcmp(argv[1],"pcm-cadence")){
        /* Before caller PCM arrives, encoded silence frames leave at the exact
         * 7.5 ms eSCO period even though the owner polls on a jittered 1 ms grid. */
        uint64_t start=wire.now,before=sco_bytes;jittered(3000000);
        uint64_t frames=(sco_bytes-before)/60,expected=(wire.now-start)/7500;
        fprintf(stderr,"Playback %llu frames over %llu us, expected %llu\n",(unsigned long long)frames,(unsigned long long)(wire.now-start),(unsigned long long)expected);
        assert(frames<=expected+1&&frames+1>=expected&&!playback.frame_restarts);
        current_task=reader;assert(bluetooth_playback_end(&app,1)==1);current_task=owner;finish_close();
        puts("PASS playback frame cadence anchored to the eSCO period");return 0;}
    if(!strcmp(argv[1],"pcm-partial")){
        current_task=reader;uint8_t hash[32];storage_digest_bytes(caller,2400,hash);
        assert(bluetooth_playback_write(&app,1,0,caller,1024)==1&&bluetooth_playback_commit(&app,1,hash)==-1&&!atomic_load(&playback.accepted));
        assert(bluetooth_playback_end(&app,1)==1);current_task=owner;finish_close();assert(!atomic_load(&playback.accepted)&&!atomic_load(&playback.slots[0].state));puts("PASS partial upload never reaches encoder");return 0;}
    upload(1);upload(2);current_task=reader;assert(bluetooth_playback_write(&app,3,0,caller,1)==0&&bluetooth_playback_ack(&app,1)==-1);current_task=owner;
    if(!strcmp(argv[1],"pcm-cancel")){current_task=reader;assert(bluetooth_playback_end(&app,1)==1);current_task=owner;finish_close();}
    else {
        playback_hold_credits=!strcmp(argv[1],"pcm-starve");
        for(unsigned k=0;k<300&&!atomic_load(&playback.slots[1].state);++k)assert(pump()>=0);
        /* Advance time in bounded owner-service increments. Inject actual encoded
         * peer frames for duplex so decoded capture traverses the same owner. */
        unsigned injected=0;uint64_t next_rx=wire.now;sbc_t encoder;assert(!sbc_init_msbc(&encoder,0));encoder.endian=SBC_LE;
        for(unsigned k=0;k<4000&&atomic_load(&playback.slots[1].state)!=2;++k){
            wire.now+=499;assert(pump()>=0);
            if(playback_fixture_mode==5&&injected<20&&wire.now>=next_rx){uint8_t f[60]={1};const uint8_t seq[]={8,0x38,0xc8,0xf8};f[1]=seq[injected&3];ssize_t written;
                assert(sbc_encode(&encoder,caller+(injected%20)*240,240,f+2,57,&written)==240&&written==57);inject(f,60,0);++injected;next_rx=wire.now+7500;}
            if(playback_hold_credits&&k==200){assert(!atomic_load(&playback.slots[0].state)||atomic_load(&playback.slots[0].state)==1);current_task=reader;assert(bluetooth_playback_end(&app,1)==1);current_task=owner;break;}
        }
        sbc_finish(&encoder);
        if(playback_hold_credits)finish_close();
        else {current_task=reader;assert(bluetooth_playback_end(&app,0)==1);current_task=owner;finish_close();}
    }
    current_task=reader;struct bluetooth_playback_result r;struct bluetooth_playback_status status;
    for(unsigned ticket=1;ticket<=2;++ticket){assert(bluetooth_playback_copy(&app,&r,&status)==1&&r.ticket==ticket&&r.codec==2);
        assert(!memcmp(r.sha256,playback.slots[ticket-1].result.sha256,32));
        if(!strcmp(argv[1],"pcm-cancel"))assert(r.flags&1);
        else if(!strcmp(argv[1],"pcm-starve"))assert(r.flags&&(r.flushed_packets||r.encoded_samples<1200));
        else assert(!r.flags&&r.encoded_samples==1200&&r.submitted_bytes==600&&r.released_packets&&r.uart_packets==r.released_packets);
        assert(bluetooth_playback_ack(&app,ticket+1)==-1&&bluetooth_playback_ack(&app,ticket)==1&&bluetooth_playback_ack(&app,ticket)==-1);}
    assert(bluetooth_playback_copy(&app,&r,&status)==0&&status.disconnected&&status.accepted==2&&status.acknowledged==2);
    if(playback_fixture_mode==5){current_task=owner;assert(atomic_load(&voice.segment_head)==2);FILE *f=fopen(getenv("SUNSTONE_DUPLEX_PCM"),"wb");assert(f);take(1,f);take(2,f);assert(!fclose(f));}
    if(playback_fixture_mode==5){char path[4096];snprintf(path,sizeof(path),"%s.outgoing.bin",getenv("SUNSTONE_DUPLEX_PCM"));FILE *f=fopen(path,"wb");assert(f&&fwrite(outgoing,1,outgoing_bytes,f)==outgoing_bytes&&!fclose(f));}
    puts("PASS distinct caller PCM periods, exact ownership and actual SCO disposition/confirmed close");return 0;
}
