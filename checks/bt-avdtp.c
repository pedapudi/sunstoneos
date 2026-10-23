/* SPDX-License-Identifier: Apache-2.0 */
/* AVDTP sink over the actual UART/H4/HCI/ACL/L2CAP workers and local SDP
 * server. AVDTP and A2DP message bytes are encoded independently here from
 * AVDTP1.3 and A2DP1.3; SBC frames come from the pinned BlueZ libsbc. */
#define main previous_l2cap_model
#include "avdtp-l2cap-fixture.h"
#undef main
#include "bt_avdtp.h"
#include "bt_sdp_server.h"
#include "sdp-ref.h"
#include "sbc/sbc.h"
#include <math.h>
struct local {struct simulation lower;struct bt_sdp_server server;struct bt_avdtp sink;unsigned next,ident;int allow;};
/* Peer signaling identifiers rotate through1..255 before any reuse. */
static unsigned ident(struct local *d){d->ident=d->ident%255+1;return d->ident;}
static int local_policy(void *p,uint64_t link,uint16_t psm,unsigned incoming)
{struct local *d=p;(void)incoming;assert(link==d->lower.link);return d->allow?(psm==1||psm==BT_AVDTP_PSM): -1;}
static int route(void *p,uint64_t link,uint16_t psm,unsigned incoming)
{struct local *d=p;return psm==1?bt_sdp_server_admit(&d->server,link,psm,incoming):bt_avdtp_admit(&d->sink,link,psm,incoming);}
static const uint8_t capabilities[]={0xff,0xff,2,53};
static int apoll_result(struct local *d)
{
    d->lower.f.room=1024;++d->lower.f.now;
    if(bt_sdp_server_step(&d->server,64,UINT64_MAX)!=1){fprintf(stderr,"lower state=%u error=%u acl=%u hci=%u h4=%u rn=%zu ri=%zu sink=%u stream=%u outputs=%u\n",
        d->lower.q.state,d->lower.q.error,d->lower.a.state,d->lower.f.hci.state,d->lower.f.hci.transport.state,d->lower.f.rn,d->lower.f.ri,d->sink.error,d->sink.stream,d->lower.outputs);assert(0);}
    wire(&d->lower);d->lower.f.room=1024;++d->lower.f.now;
    int r=bt_avdtp_step(&d->sink,64,UINT64_MAX);wire(&d->lower);return r;
}
static void apoll(struct local *d){int r=apoll_result(d);if(r<0)fprintf(stderr,"AVDTP error %u\n",d->sink.error);assert(r>=0);}
static unsigned out(struct local *d)
{
    for(unsigned i=0;i<400 && d->next==d->lower.outputs;++i)apoll(d);
    assert(d->next<d->lower.outputs);unsigned o=d->next++;
    for(unsigned i=0;i<5;++i)apoll(d);return o;
}
static void quiet(struct local *d){for(unsigned i=0;i<40;++i)apoll(d);assert(d->next==d->lower.outputs);}
static void start(struct local *d)
{
    memset(d,0,sizeof(*d));ready(&d->lower.f,&d->lower.a);
    d->lower.link=begin_connect(&d->lower.f,&d->lower.a,1);
    connected(&d->lower.f,&d->lower.a,d->lower.link,0x23);
    d->lower.wire_cursor=d->lower.f.tn;d->lower.automatic_credits=1;d->allow=1;
    const struct bt_sdp_ag_profile p={0x10001,0,0x109,0,7,0,0x10002,1};
    assert(bt_sdp_server_init(&d->server,&d->lower.q,&p,1000,5000,local_policy,d)==1);
    assert(bt_avdtp_init(&d->sink,&d->lower.q,capabilities,100000,local_policy,d)==1);
    assert(bt_l2cap_init(&d->lower.q,&d->lower.a,BT_L2CAP_MTU,1000000,60000000,route,d)==1);
}
/* Peer-initiated channel; returns the local CID or 0 after a refusal. */
static unsigned connect_channel(struct local *d,unsigned psm,unsigned remote,unsigned expected_result)
{
    const struct l2cap_conn_req req={(uint16_t)psm,(uint16_t)remote};signal_in(&d->lower,L2CAP_CONN_REQ,ident(d),&req,4);
    const struct l2cap_cmd_hdr *c=command_out(&d->lower,out(d),L2CAP_CONN_RSP);
    struct l2cap_conn_rsp rsp;memcpy(&rsp,c+1,sizeof(rsp));
    assert(rsp.scid==remote && rsp.result==expected_result);if(expected_result)return 0;
    c=command_out(&d->lower,out(d),L2CAP_CONF_REQ);
    const uint8_t accepted[]={(uint8_t)rsp.dcid,(uint8_t)(rsp.dcid>>8),0,0,0,0};signal_in(&d->lower,L2CAP_CONF_RSP,c->ident,accepted,6);
    const uint8_t mtu[]={(uint8_t)rsp.dcid,(uint8_t)(rsp.dcid>>8),0,0,1,2,0,4};signal_in(&d->lower,L2CAP_CONF_REQ,ident(d),mtu,8);
    command_out(&d->lower,out(d),L2CAP_CONF_RSP);return rsp.dcid;
}
static void disconnect_channel(struct local *d,unsigned local,unsigned remote)
{
    const uint8_t req[]={(uint8_t)local,(uint8_t)(local>>8),(uint8_t)remote,(uint8_t)(remote>>8)};
    signal_in(&d->lower,L2CAP_DISCONN_REQ,ident(d),req,4);command_out(&d->lower,out(d),L2CAP_DISCONN_RSP);
    for(unsigned i=0;i<10;++i)apoll(d);
}
struct peer {unsigned signal,remote,media,media_remote,label;};
static void command(struct local *d,struct peer *p,unsigned signal,const uint8_t *body,unsigned n)
{
    uint8_t m[64]={(uint8_t)(p->label<<4),(uint8_t)signal};assert(n<=62);if(n)memcpy(m+2,body,n);
    incoming_pdu(&d->lower,p->signal,m,n+2,0);
}
/* The reply's header is label, single packet and message type. */
static void expect(struct local *d,struct peer *p,unsigned type,unsigned signal,const uint8_t *body,unsigned n)
{
    unsigned o=out(d);struct l2cap_hdr h;memcpy(&h,d->lower.output[o],4);const uint8_t *r=d->lower.output[o]+4;
    if(h.len!=n+2 || r[0]!=(p->label<<4|type) || r[1]!=signal || (n && memcmp(r+2,body,n))){
        fprintf(stderr,"reply:");for(unsigned i=0;i<h.len;++i)fprintf(stderr," %02x",r[i]);fprintf(stderr,"\n");}
    assert(h.cid==p->remote && h.len==n+2 && r[0]==(p->label<<4|type) && r[1]==signal && (!n || !memcmp(r+2,body,n)));
    p->label=(p->label+1)&15;
}
static void exchange(struct local *d,struct peer *p,unsigned signal,const uint8_t *body,unsigned n,
    unsigned type,const uint8_t *reply,unsigned rn)
{command(d,p,signal,body,n);expect(d,p,type,signal,reply,rn);}
enum {GENERAL=1,ACCEPT,REJECT};
static void configure(struct local *d,struct peer *p,const uint8_t c[4])
{
    const uint8_t set[]={4,8,1,0,7,6,0,0,c[0],c[1],c[2],c[3]};exchange(d,p,3,set,sizeof(set),ACCEPT,0,0);
    const uint8_t seid[]={4};exchange(d,p,6,seid,1,ACCEPT,0,0);assert(d->sink.stream==BT_AVDTP_OPEN);
    p->media=connect_channel(d,BT_AVDTP_PSM,p->media_remote,0);
    exchange(d,p,7,seid,1,ACCEPT,0,0);assert(d->sink.stream==BT_AVDTP_STREAMING);
}
static void abort_stream(struct local *d,struct peer *p)
{
    const uint8_t seid[]={4};exchange(d,p,10,seid,1,ACCEPT,0,0);assert(d->sink.stream==BT_AVDTP_ABORTING);
    disconnect_channel(d,p->media,p->media_remote);assert(d->sink.stream==BT_AVDTP_IDLE && !d->sink.media);
}
static unsigned media_packet(uint8_t *packet,unsigned sequence,uint32_t timestamp,const uint8_t *frames,unsigned count,unsigned n)
{
    const uint8_t rtp[]={0x80,0x60,(uint8_t)(sequence>>8),(uint8_t)sequence,(uint8_t)(timestamp>>24),(uint8_t)(timestamp>>16),
        (uint8_t)(timestamp>>8),(uint8_t)timestamp,1,2,3,4,(uint8_t)count};
    memcpy(packet,rtp,13);memcpy(packet+13,frames,n);return n+13;
}
/* Encode a stereo or mono sine with the configured parameters. */
static unsigned encode(const uint8_t c[4],unsigned bitpool,unsigned frames,uint8_t *output,size_t capacity,int16_t *pcm,size_t *samples)
{
    sbc_t sbc;assert(sbc_init_a2dp(&sbc,0,c,4)==0);sbc.bitpool=(uint8_t)bitpool;sbc.endian=SBC_LE;
    size_t code=sbc_get_codesize(&sbc),used=0;unsigned channels=sbc.mode==SBC_MODE_MONO?1:2;
    *samples=0;
    for(unsigned f=0;f<frames;++f){int16_t block[512];assert(code<=sizeof(block));
        for(size_t i=0;i<code/2;++i){size_t t=(*samples+i)/channels;block[i]=(int16_t)(8000*sin(2*M_PI*440.0*(double)t/44100.0));}
        memcpy(pcm+*samples,block,code);*samples+=code/2;
        ssize_t written=0;assert(sbc_encode(&sbc,block,code,output+used,capacity-used,&written)==(ssize_t)code);
        assert(written==(ssize_t)sbc_get_frame_length(&sbc));used+=(size_t)written;}
    sbc_finish(&sbc);return (unsigned)used;
}
static void open_signaling(struct local *d,struct peer *p)
{
    *p=(struct peer){.remote=0x70,.media_remote=0x71};
    p->signal=connect_channel(d,BT_AVDTP_PSM,p->remote,0);
    assert(d->sink.signal && d->sink.link==d->lower.link);
}
static void signaling_and_media(void)
{
    static struct local d;start(&d);struct peer p;unsigned n;
    /* The A2DP sink record is found through the composed SDP server first. */
    const unsigned sdp=connect_channel(&d,1,0x60,0);
    const uint8_t query[]={SDP_SVC_SEARCH_ATTR_REQ,0,1,0,13,SDP_SEQ8,3,SDP_UUID16,0x11,0x0b,0,64,SDP_SEQ8,3,SDP_UINT16,0,1,0};
    incoming_pdu(&d.lower,sdp,query,sizeof(query),0);n=out(&d);
    const uint8_t found[]={0x35,10,0x35,8,9,0,1,0x35,3,0x19,0x11,0x0b};
    assert(d.lower.output[n][4]==SDP_SVC_SEARCH_ATTR_RSP && !memcmp(d.lower.output[n]+11,found,sizeof(found)));
    open_signaling(&d,&p);
    const uint8_t endpoint[]={4,8},busy[]={6,8},seid[]={4};
    exchange(&d,&p,1,0,0,ACCEPT,endpoint,2);
    const uint8_t caps[]={1,0,7,6,0,0,0xff,0xff,2,53};
    exchange(&d,&p,2,seid,1,ACCEPT,caps,sizeof(caps));exchange(&d,&p,12,seid,1,ACCEPT,caps,sizeof(caps));
    const uint8_t wrong[]={8};const uint8_t bad_seid[]={0x12};exchange(&d,&p,2,wrong,1,REJECT,bad_seid,1);
    const uint8_t bad_length[]={0x11};exchange(&d,&p,1,seid,1,REJECT,bad_length,1);
    /* Configuration refusals name the offending category and A2DP error. */
    const struct {uint8_t set[16];unsigned n;uint8_t reply[2];} refused[]={
        {{4,8,1,0,7,6,0,0,0x31,0x15,2,53},12,{7,0xc3}},{{4,8,1,0,7,6,0,0,0x23,0x15,2,53},12,{7,0xc5}},
        {{4,8,1,0,7,6,0,0,0x21,0x35,2,53},12,{7,0xdd}},{{4,8,1,0,7,6,0,0,0x21,0x1d,2,53},12,{7,0xc7}},
        {{4,8,1,0,7,6,0,0,0x21,0x17,2,53},12,{7,0xc9}},{{4,8,1,0,7,6,0,0,0x21,0x15,1,53},12,{7,0xcb}},
        {{4,8,1,0,7,6,0,0,0x21,0x15,2,251},12,{7,0xcd}},{{4,8,1,0,7,6,0,0,0x21,0x15,2,54},12,{7,0xce}},
        {{4,8,1,0,7,6,0,0,0x21,0x15,20,10},12,{7,0xcd}},{{4,8,1,0,7,6,0,2,0x21,0x15,2,53},12,{7,0xc2}},
        {{4,8,7,6,0,0,0x21,0x15,2,53},10,{1,0x1a}},
        {{4,8,1,0},4,{7,0x1a}},{{4,8,1,0,7,6,0,0,0x21,0x15,2,53,8,0},14,{8,0x29}},
        {{4,8,1,0,7,6,0,0,0x21,0x15,2,53,4,2,2,0},16,{4,0x29}},{{4,8,1,1,0,7,6,0,0,0x21,0x15,2,53},13,{1,0x23}},
        {{4,8,9,0},4,{9,0x17}},{{4,8,1,0,1,0},6,{1,0x18}},{{4,8,1,0,7,9,0},7,{7,0x18}},
        {{8,8,1,0,7,6,0,0,0x21,0x15,2,53},12,{0,0x12}},{{4,0,1,0,7,6,0,0,0x21,0x15,2,53},12,{0,0x18}},{{4},1,{0,0x11}}};
    for(unsigned i=0;i<sizeof(refused)/sizeof(refused[0]);++i){
        exchange(&d,&p,3,refused[i].set,refused[i].n,REJECT,refused[i].reply,2);assert(d.sink.stream==BT_AVDTP_IDLE);}
    const uint8_t not_in_use[]={0x14};exchange(&d,&p,4,seid,1,REJECT,not_in_use,1);
    const uint8_t state[]={0x31},start_state[]={4,0x31};
    exchange(&d,&p,6,seid,1,REJECT,state,1);
    const uint8_t joint[]={0x21,0x15,2,53},set[]={4,8,1,0,7,6,0,0,0x21,0x15,2,53};
    exchange(&d,&p,3,set,sizeof(set),ACCEPT,0,0);assert(d.sink.stream==BT_AVDTP_CONFIGURED && d.sink.int_seid==2);
    const uint8_t in_use[]={0,0x13};exchange(&d,&p,3,set,sizeof(set),REJECT,in_use,2);
    exchange(&d,&p,1,0,0,ACCEPT,busy,2);
    const uint8_t configured[]={1,0,7,6,0,0,0x21,0x15,2,53};exchange(&d,&p,4,seid,1,ACCEPT,configured,sizeof(configured));
    exchange(&d,&p,7,seid,1,REJECT,start_state,2);
    /* No transport channel exists before OPEN is accepted. */
    connect_channel(&d,BT_AVDTP_PSM,0x71,2);
    exchange(&d,&p,6,seid,1,ACCEPT,0,0);exchange(&d,&p,7,seid,1,REJECT,start_state,2);
    const uint8_t reconfigure[]={4,7,6,0,0,0x21,0x15,2,53},unsupported[]={7,0x29};
    exchange(&d,&p,5,reconfigure,sizeof(reconfigure),REJECT,unsupported,2);
    p.media=connect_channel(&d,BT_AVDTP_PSM,0x71,0);assert(d.sink.media);
    connect_channel(&d,BT_AVDTP_PSM,0x72,2);
    exchange(&d,&p,7,seid,1,ACCEPT,0,0);assert(d.sink.stream==BT_AVDTP_STREAMING);
    exchange(&d,&p,13,0,0,GENERAL,0,0);
    /* An ABORT for another endpoint receives no response and changes nothing. */
    command(&d,&p,10,wrong,1);quiet(&d);assert(d.sink.stream==BT_AVDTP_STREAMING);
    /* Media: copied SBC frames, RTP sequence gaps and decoded audio. */
    static uint8_t frames[8192],packet[1100];static int16_t pcm[65536],decoded[65536];size_t samples;
    unsigned bytes=encode(joint,35,28,frames,sizeof(frames),pcm,&samples),frame=bytes/28;assert(frame==83);
    unsigned sequence[]={100,101,103,104};
    for(unsigned k=0;k<4;++k){unsigned m=media_packet(packet,sequence[k],128*7*k,frames+frame*7*k,7,frame*7);
        incoming_pdu(&d.lower,p.media,packet,m,0);for(unsigned i=0;i<6;++i)apoll(&d);}
    sbc_t sbc;assert(sbc_init_a2dp(&sbc,0,d.sink.configuration,4)==0);size_t total=0;
    for(unsigned k=0;k<4;++k){const struct bt_avdtp_media *m=bt_avdtp_media(&d.sink);
        assert(m && m->sequence==sequence[k] && m->frames==7 && m->length==frame*7 && m->timestamp==128*7*k);
        assert(m->gap==(k==2) && !memcmp(m->bytes,frames+frame*7*k,frame*7));
        for(unsigned at=0;at<m->length;){size_t written=0;
            ssize_t used=sbc_decode(&sbc,m->bytes+at,m->length-at,decoded+total,sizeof(decoded)-2*total,&written);
            assert(used==(ssize_t)frame && written==512);at+=(unsigned)used;total+=written/2;}
        assert(bt_avdtp_release_media(&d.sink,m->serial)==1 && bt_avdtp_release_media(&d.sink,m->serial)==-1);}
    sbc_finish(&sbc);assert(total==samples && d.sink.gaps==1 && !bt_avdtp_media(&d.sink));
    /* The decoded sine matches the input after the codec delay. */
    double signal=0,noise=0;unsigned delay=0;double best=1e30;
    for(unsigned shift=0;shift<600;shift+=2){double e=0;for(size_t i=4000;i<8000;++i){double v=decoded[i+shift]-pcm[i];e+=v*v;}
        if(e<best){best=e;delay=shift;}}
    for(size_t i=4000;i<samples-delay-1000;++i){signal+=(double)pcm[i]*pcm[i];double v=decoded[i+delay]-pcm[i];noise+=v*v;}
    assert(10*log10(signal/noise)>25);
    /* Invalid media is counted and never published. */
    unsigned m=media_packet(packet,105,0,frames,7,frame*7);packet[13]=0x9d;
    incoming_pdu(&d.lower,p.media,packet,m,0);for(unsigned i=0;i<6;++i)apoll(&d);
    m=media_packet(packet,105,0,frames,7,frame*7);packet[12]|=0x80;
    incoming_pdu(&d.lower,p.media,packet,m,0);for(unsigned i=0;i<6;++i)apoll(&d);
    m=media_packet(packet,105,0,frames,6,frame*7);
    incoming_pdu(&d.lower,p.media,packet,m,0);for(unsigned i=0;i<6;++i)apoll(&d);
    static uint8_t excessive[512];unsigned high=encode(joint,60,1,excessive,sizeof(excessive),pcm,&samples);
    media_packet(packet,105,0,excessive,1,high);
    incoming_pdu(&d.lower,p.media,packet,13+high,0);for(unsigned i=0;i<6;++i)apoll(&d);
    assert(d.sink.invalid==4 && !bt_avdtp_media(&d.sink));
    /* Four slots hold copies; a fifth packet is dropped and reported as a gap. */
    for(unsigned k=0;k<6;++k){m=media_packet(packet,(unsigned)(105+k),0,frames,1,frame);
        incoming_pdu(&d.lower,p.media,packet,m,0);for(unsigned i=0;i<6;++i)apoll(&d);
        if(k==4){assert(d.sink.dropped==1);for(unsigned j=0;j<4;++j){const struct bt_avdtp_media *x=bt_avdtp_media(&d.sink);
            assert(x->sequence==105+j && !x->gap);assert(bt_avdtp_release_media(&d.sink,x->serial)==1);}}}
    assert(bt_avdtp_media(&d.sink)->sequence==110 && bt_avdtp_media(&d.sink)->gap==1);
    bt_avdtp_release_media(&d.sink,bt_avdtp_media(&d.sink)->serial);
    /* Suspended streams discard media without publication. */
    exchange(&d,&p,9,seid,1,ACCEPT,0,0);assert(d.sink.stream==BT_AVDTP_OPEN);
    incoming_pdu(&d.lower,p.media,packet,m,0);for(unsigned i=0;i<6;++i)apoll(&d);assert(!bt_avdtp_media(&d.sink));
    exchange(&d,&p,9,seid,1,REJECT,start_state,2);exchange(&d,&p,7,seid,1,ACCEPT,0,0);
    const uint8_t two[]={4,8};const uint8_t two_reply[]={8,0x12};exchange(&d,&p,9,two,2,REJECT,two_reply,2);
    /* CLOSE: the source then closes the transport channel. */
    exchange(&d,&p,8,seid,1,ACCEPT,0,0);assert(d.sink.stream==BT_AVDTP_CLOSING);
    disconnect_channel(&d,p.media,0x71);assert(d.sink.stream==BT_AVDTP_IDLE && !d.sink.media);
    exchange(&d,&p,1,0,0,ACCEPT,endpoint,2);
    /* Every channel mode, block length and subband count frames correctly. */
    const uint8_t freq[]={0x80,0x40,0x20,0x10},mode[]={8,4,2,1},block[]={0x80,0x40,0x20,0x10},band[]={8,4},alloc[]={2,1};
    unsigned configs=0;p.media_remote=0x80;
    for(unsigned f=0;f<4;++f)for(unsigned c=0;c<4;++c)for(unsigned b=0;b<4;++b)for(unsigned s=0;s<2;++s){
        const uint8_t chosen[]={(uint8_t)(freq[f]|mode[c]),(uint8_t)(block[b]|band[s]|alloc[(f+c+b+s)&1]),2,53};
        configure(&d,&p,chosen);unsigned pool=2+(f*32+c*8+b*2+s)%52;
        bytes=encode(chosen,pool,3,frames,sizeof(frames),pcm,&samples);
        m=media_packet(packet,7,0,frames,3,bytes);incoming_pdu(&d.lower,p.media,packet,m,0);for(unsigned i=0;i<6;++i)apoll(&d);
        const struct bt_avdtp_media *x=bt_avdtp_media(&d.sink);assert(x && x->length==bytes && x->frames==3);
        bt_avdtp_release_media(&d.sink,x->serial);abort_stream(&d,&p);++p.media_remote;++configs;}
    assert(configs==128 && d.sink.invalid==4);
    /* A peer that closes signaling returns the sink to accepting a new peer. */
    exchange(&d,&p,3,set,sizeof(set),ACCEPT,0,0);
    disconnect_channel(&d,p.signal,p.remote);assert(!d.sink.signal && !d.sink.link && d.sink.stream==BT_AVDTP_IDLE);
    open_signaling(&d,&p);exchange(&d,&p,1,0,0,ACCEPT,endpoint,2);
    puts("AVDTP sink discovery, configuration refusals, open/start/suspend/close/abort, copied SBC media, gaps and 128 frame layouts pass");
}
static void local_close(void)
{
    static struct local d;start(&d);struct peer p;open_signaling(&d,&p);
    const uint8_t joint[]={0x21,0x15,2,53};configure(&d,&p,joint);
    assert(bt_avdtp_close(&d.sink,d.lower.f.now+50000)==1 && bt_avdtp_close(&d.sink,d.lower.f.now+50000)==-1);
    /* Media closes first, then signaling; both are released after replies. */
    const struct l2cap_cmd_hdr *c=command_out(&d.lower,out(&d),L2CAP_DISCONN_REQ);
    uint8_t reply[4];memcpy(reply,c+1,4);assert(reply[0]==0x71);
    quiet(&d);assert(d.lower.q.channels[0].state==BT_L2CAP_OPEN);
    signal_in(&d.lower,L2CAP_DISCONN_RSP,c->ident,reply,4);
    c=command_out(&d.lower,out(&d),L2CAP_DISCONN_REQ);memcpy(reply,c+1,4);assert(reply[0]==0x70);
    signal_in(&d.lower,L2CAP_DISCONN_RSP,c->ident,reply,4);
    int r=1;for(unsigned i=0;i<20 && r;++i)r=apoll_result(&d);
    assert(!r && !d.sink.signal && !d.sink.media);
    for(unsigned i=0;i<BT_L2CAP_CHANNELS;++i)assert(d.lower.q.channels[i].state==BT_L2CAP_UNUSED);
    const struct l2cap_conn_req req={BT_AVDTP_PSM,0x90};signal_in(&d.lower,L2CAP_CONN_REQ,ident(&d),&req,4);
    c=command_out(&d.lower,out(&d),L2CAP_CONN_RSP);struct l2cap_conn_rsp rsp;memcpy(&rsp,c+1,sizeof(rsp));assert(rsp.result==2);
    puts("AVDTP local close releases media before signaling and refuses new streams");
}
static void refusals(void)
{
    static struct local d;start(&d);struct peer p;
    const uint8_t missing[]={0xef,0xff,2,53},range[]={0xff,0xff,40,30},pool[]={0xff,0xff,1,53},blocks[]={0xff,0x7f,2,53};
    struct bt_avdtp other={0};struct bt_l2cap lower={0};
    assert(bt_avdtp_init(&other,&lower,missing,1000,local_policy,&d)==-1 && bt_avdtp_init(&other,&lower,range,1000,local_policy,&d)==-1);
    assert(bt_avdtp_init(&other,&lower,pool,1000,local_policy,&d)==-1 && bt_avdtp_init(&other,&lower,blocks,1000,local_policy,&d)==-1);
    assert(bt_avdtp_init(&other,&lower,capabilities,0,local_policy,&d)==-1 && bt_avdtp_init(&other,&lower,capabilities,60000001,local_policy,&d)==-1);
    assert(bt_avdtp_admit(&d.sink,d.lower.link,BT_AVDTP_PSM,0)==-1);
    d.allow=0;connect_channel(&d,BT_AVDTP_PSM,0x70,3);assert(!d.sink.signal);d.allow=1;
    open_signaling(&d,&p);
    /* A fragmented start packet is refused and the fault is retained. */
    const uint8_t start_packet[]={(uint8_t)(1<<4|1<<2),2,3,0x01};incoming_pdu(&d.lower,p.signal,start_packet,4,0);
    int r=1;for(unsigned i=0;i<20 && r>=0;++i)r=apoll_result(&d);
    assert(r==-1 && d.sink.error==BT_AVDTP_PROTOCOL && bt_avdtp_step(&d.sink,64,UINT64_MAX)==-1);
    /* The fault refuses further sinks but still delegates other services. */
    assert(bt_avdtp_admit(&d.sink,d.lower.link,BT_AVDTP_PSM,1)==-1 && bt_avdtp_admit(&d.sink,d.lower.link,1,1)==1);
    /* Revoked security while streaming stops commands, replies and media at
     * the next step and retains both channels and their receive records. */
    start(&d);open_signaling(&d,&p);const uint8_t joint[]={0x21,0x15,2,53};configure(&d,&p,joint);
    static uint8_t frames[1024],packet[1100];static int16_t pcm[4096];size_t samples;
    unsigned bytes=encode(joint,35,1,frames,sizeof(frames),pcm,&samples),m=media_packet(packet,1,0,frames,1,bytes);
    incoming_pdu(&d.lower,p.media,packet,m,0);for(unsigned i=0;i<6;++i)apoll(&d);
    const struct bt_avdtp_media *x=bt_avdtp_media(&d.sink);assert(x);bt_avdtp_release_media(&d.sink,x->serial);
    quiet(&d);d.allow=0;
    const uint8_t seid[]={4};command(&d,&p,9,seid,1);incoming_pdu(&d.lower,p.media,packet,m,0);
    r=1;for(unsigned i=0;i<20 && r>=0;++i)r=apoll_result(&d);
    assert(r==-1 && d.sink.error==BT_AVDTP_SECURITY && d.sink.stream==BT_AVDTP_STREAMING && !bt_avdtp_media(&d.sink));
    assert(d.next==d.lower.outputs && d.sink.signal && d.sink.media && !d.sink.response_length && !d.sink.tx_serial);
    unsigned held=0;for(unsigned i=0;i<BT_L2CAP_PDUS;++i)held+=d.lower.q.rx[i].state==BT_L2CAP_RECEIVED;assert(held==2);
    assert(bt_avdtp_close(&d.sink,d.lower.f.now+50000)==-1);
    /* An unretired response expires at its deadline. */
    start(&d);open_signaling(&d,&p);d.lower.automatic_credits=0;command(&d,&p,1,0,0);
    for(unsigned i=0;i<40 && !d.sink.tx_serial;++i)apoll(&d);assert(d.sink.tx_serial);
    d.lower.f.now=d.sink.deadline;r=apoll_result(&d);assert(r==-1 && d.sink.error==BT_AVDTP_EXPIRED);
    puts("AVDTP refuses invalid capabilities, outgoing and denied channels, fragmented signaling, revoked security and expired responses");
}
int main(void)
{
    signaling_and_media();local_close();refusals();
    puts("AVDTP SBC sink passes over actual UART/HCI/ACL/L2CAP with the local SDP sink record");return 0;
}
