/* The real finite application/HCI/security/L2CAP/SDP/AVDTP/decoder path.
 * Providers and peer radio packets are modeled; no board is accessed. */
#include <stdlib.h>
#define BLUETOOTH_CHECK_A2DP 1
#define BLUETOOTH_HEADSET_FIXTURE_ENTRY previous_headset_main
#include "bluetooth-headset.c"
#undef BLUETOOTH_HEADSET_FIXTURE_ENTRY
#include "../apps/bluetooth/a2dp.h"
static uint8_t output[256][1100];static unsigned lengths[256],head,tail,identifier;
static void a2dp_peer_acl(const uint8_t *p,unsigned n)
{assert(tail<256&&n<=1100);memcpy(output[tail],p,n);lengths[tail++]=n;}
static const uint8_t *next(unsigned code)
{
    for(unsigned i=0;i<400&&head==tail;++i)assert(pump()>=0);
    assert(head<tail);const uint8_t *p=output[head++];
    assert(get16(p+2)==1&&p[4]==code);return p+4;
}
static unsigned channel(unsigned remote,unsigned psm)
{
    uint8_t body[4];put16(body,psm);put16(body+2,remote);sig_in(2,++identifier,body,4);
    const uint8_t *p=next(3);assert(get16(p+8)==0);unsigned local=get16(p+4);
    p=next(4);uint8_t answer[6];put16(answer,local);memset(answer+2,0,4);sig_in(5,p[1],answer,6);
    uint8_t config[8];put16(config,local);memset(config+2,0,2);config[4]=1;config[5]=2;put16(config+6,512);
    sig_in(4,++identifier,config,8);p=next(5);assert(get16(p+8)==0);return local;
}
static void exchange(unsigned cid,unsigned signal,const uint8_t *body,unsigned n)
{
    uint8_t command[32]={(uint8_t)((++identifier%16)<<4),(uint8_t)signal};assert(n<=30);
    memcpy(command+2,body,n);acl_in(cid,command,n+2);
    for(unsigned i=0;i<400&&head==tail;++i)assert(pump()>=0);
    assert(head<tail);const uint8_t *p=output[head++];
    assert(get16(p+2)==0x70&&(p[4]&3)==2&&p[5]==signal);
}
static void close_one(unsigned local,unsigned remote)
{
    const uint8_t *p=next(6);assert(get16(p+4)==remote&&get16(p+6)==local);
    uint8_t body[4];memcpy(body,p+4,4);sig_in(7,p[1],body,4);
}
static uint8_t identity_frame[1296];
static unsigned identity_bytes,identity_sent,identity_transferred;
static struct {struct {uint64_t epoch,generation;uint8_t sid_evidence[32];} session;} capture_board;
static void put32(uint8_t *p,uint32_t x){for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(x>>(i*8));}
static void put64(uint8_t *p,uint64_t x){for(unsigned i=0;i<8;++i)p[i]=(uint8_t)(x>>(i*8));}
static void stop(void){abort();}
#define bluetooth app
#define board capture_board
#include "a2dp-pcm-exchange.h"
#undef board
#undef bluetooth
static void save_bytes(const char *directory,const char *name,const void *data,size_t n)
{char path[4096];assert(snprintf(path,sizeof(path),"%s/%s",directory,name)>0);FILE *f=fopen(path,"wb");assert(f);assert(fwrite(data,1,n,f)==n);assert(!fclose(f));}
static void export_capture(const char *directory)
{
    uint8_t session[592]={0};put64(session,UINT64_C(0x4153373342535332));put64(session+8,2);put64(session+16,592);
    capture_board.session.epoch=31;capture_board.session.generation=37;
    storage_digest_bytes(&profile,profile.bytes,capture_board.session.sid_evidence);
    put64(session+24,31);put64(session+32,37);put64(session+312,profile.sid_authority);put32(session+400,2);
    memcpy(session+320,capture_board.session.sid_evidence,32);
    save_bytes(directory,"session.bin",session,sizeof(session));save_bytes(directory,"profile.bin",&profile,profile.bytes);
    for(unsigned kind=0;kind<2;++kind){unsigned total=kind?voice.snapshot.records*24:voice.snapshot.bytes;
        for(unsigned offset=0;offset<total;offset+=1024){unsigned n=total-offset;if(n>1024)n=1024;
            uint8_t request[8]={(uint8_t)(kind+1),0,(uint8_t)n,(uint8_t)(n>>8)};put32(request+4,offset);
            pcm_exchange(request);assert(identity_bytes==n+160&&identity_frame[8]==1);
            char name[80];snprintf(name,sizeof(name),"frame-%u-%u.bin",kind,offset);save_bytes(directory,name,identity_frame,identity_bytes);}}
}
int main(int argc,char **argv)
{
    assert(argc==4||argc==5);FILE *f=fopen(argv[2],"rb");assert(f);uint8_t encoded[512];size_t bytes_read=fread(encoded,1,sizeof(encoded),f);assert(!ferror(f)&&feof(f));fclose(f);
    unsigned short_frames=!strcmp(argv[3],"short"),single=!strcmp(argv[3],"single");
    if(single){short_frames=1;bytes_read/=15;}
    assert(bluetooth_a2dp_init()==1&&!bluetooth_a2dp_init());setup();
    if(short_frames||!strcmp(argv[1],"gap")||!strcmp(argv[1],"revocation")||!strcmp(argv[1],"peer-close"))profile.headset.samples=24000;
    /* Rebind the immutable profile after the setup helper's default size. */
    uint8_t digest[32];storage_digest_bytes(&profile,profile.bytes,digest);assert(bluetooth_profile_valid(&profile,digest));
    until(CONNECT);const uint8_t request[]={1,2,3,4,5,6,4,4,0x24,1};event(&wire,4,request,sizeof(request));until(CAPTURE);
    assert(!sco_setups&&voice.sink.initialized&&voice.server.profile.sink_handle);
    unsigned sdp=channel(0x60,1);
    uint8_t query[]={6,0,1,0,13,0x35,3,0x19,0x11,0x1f,0,64,0x35,3,9,0,1,0};
    acl_in(sdp,query,sizeof(query));for(unsigned i=0;i<400&&head==tail;++i)assert(pump()>=0);
    assert(head<tail&&output[head][4]==7&&output[head][11]==0x35&&output[head][12]==0);++head;
    query[9]=0x0b;query[2]=2;acl_in(sdp,query,sizeof(query));for(unsigned i=0;i<400&&head==tail;++i)assert(pump()>=0);
    assert(head<tail&&output[head][4]==7&&output[head][11]==0x35&&output[head][12]==10);++head;
    unsigned signaling=channel(0x70,25);uint8_t chosen[12]={4,8,1,0,7,6,0,0,0x11,0x15,2,53};
    if(short_frames){chosen[8]=0x18;chosen[9]=0x89;}
    exchange(signaling,3,chosen,12);const uint8_t seid[]={4};exchange(signaling,6,seid,1);
    unsigned media=channel(0x71,25);exchange(signaling,7,seid,1);
    unsigned packets=single?1500:short_frames?100:1;
    for(unsigned k=0;k<packets;++k){
        uint8_t packet[540]={0x80,0x60,(uint8_t)(k>>8),(uint8_t)k,0,0,0,0,1,2,3,4,(uint8_t)(single?1:short_frames?15:3)};
        memcpy(packet+13,encoded,bytes_read);
        if(!strcmp(argv[1],"crc"))packet[16]^=1;
        if(!strcmp(argv[1],"invalid"))packet[12]|=0x80;
        if(single){
            if(k%3==0){for(unsigned j=0;j<3;++j){packet[2]=(uint8_t)((k+j)>>8);packet[3]=(uint8_t)(k+j);acl_in(media,packet,(unsigned)bytes_read+13);}
                /* Three mandatory minimum frames arrive per 1 ms owner
                 * tick; the production owner permits four polls per tick. */
                wire.now+=996;}
        }else acl_in(media,packet,(unsigned)bytes_read+13);
        unsigned records=voice.snapshot.records;
        for(unsigned i=0;i<300&&voice.snapshot.records==records&&!atomic_load(&app.failed);++i){
            int r=pump();if(r<0)break;}
        if(!strcmp(argv[1],"crc")||!strcmp(argv[1],"invalid")){
            assert(atomic_load(&app.failed)&&atomic_load(&voice.terminal)==2&&voice.sink.media);puts("PASS damaged media retains unpublished capture");return 0;}
        assert(voice.snapshot.records==records+1);
    }
    if(!strcmp(argv[1],"gap")||!strcmp(argv[1],"revocation")||!strcmp(argv[1],"peer-close")){
        if(!strcmp(argv[1],"gap")){
            uint8_t packet[540]={0x80,0x60,0,3,0,0,0,0,1,2,3,4,3};memcpy(packet+13,encoded,bytes_read);acl_in(media,packet,(unsigned)bytes_read+13);
        }else if(!strcmp(argv[1],"revocation")){
            const uint8_t lost[]={0,0x23,0,0};event(&wire,8,lost,sizeof(lost));
        }else{
            uint8_t body[4];put16(body,media);put16(body+2,0x71);sig_in(6,++identifier,body,4);
        }
        for(unsigned i=0;i<300&&!atomic_load(&app.failed);++i){if(!strcmp(argv[1],"peer-close"))wire.now+=999;(void)pump();}
        if(!atomic_load(&app.failed)&&!strcmp(argv[1],"peer-close")){
            while(wire.now<voice.capture_deadline&&!atomic_load(&app.failed)){wire.now+=999;(void)pump();}}
        assert(atomic_load(&app.failed)&&atomic_load(&voice.terminal)==2);puts("PASS discontinuity/security/early-close retains incomplete capture");return 0;
    }
    assert(voice.stage==CLOSE&&!atomic_load(&voice.terminal));
    uint8_t copied[1024];size_t count;struct bluetooth_pcm_snapshot snapshot;
    assert(bluetooth_headset_copy(&app,0,0,4,copied,&count,&snapshot)==0);
    close_one(media,0x71);close_one(signaling,0x70);until(DONE);
    assert(bluetooth_headset_copy(&app,0,0,1024,copied,&count,&snapshot)==1&&count==1024);
    assert(snapshot.bytes==profile.headset.samples*2&&snapshot.records==packets&&snapshot.state==3);
    assert(bluetooth_a2dp_rate()==48000&&bluetooth_a2dp_channels()==(short_frames?1:2));
    assert(!voice.sink.media&&!voice.sink.signal&&!snapshot.loss_packets);
    wire.now=59500001;assert(pump()==1&&atomic_load(&voice.terminal)==1);
    if(argc==5)export_capture(argv[4]);
    unsigned nonzero=0;for(unsigned i=0;i<snapshot.bytes;++i)nonzero|=voice.pcm[i];assert(nonzero);
    printf("PASS real native A2DP path: %u packets, %u PCM bytes; confirmed closure before immutable copy\n",packets,snapshot.bytes);
    return 0;
}
