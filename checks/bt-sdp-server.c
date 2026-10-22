#define main previous_l2cap_model
#include "bt-l2cap.c"
#undef main
#include "bt_sdp_server.h"
#include "bt_rfcomm.h"
#include "sdp-ref.h"
struct local {struct simulation lower;struct bt_sdp_server server;struct bt_rfcomm_registry registry;unsigned next;int allow;};
static int local_policy(void *p,uint64_t link,uint16_t psm,unsigned incoming)
{struct local *d=p;(void)incoming;assert(link==d->lower.link);return d->allow?(psm==1||psm==3): -1;}
static int service_policy(void *p,uint64_t link,uint16_t uuid,uint8_t channel)
{(void)p;(void)link;(void)uuid;(void)channel;return -1;}
static int route(void *p,uint64_t link,uint16_t psm,unsigned incoming)
{return bt_sdp_server_admit(&((struct local *)p)->server,link,psm,incoming);}
static void spoll(struct local *d)
{
    d->lower.f.room=1024;++d->lower.f.now;
    assert(bt_sdp_server_step(&d->server,64,UINT64_MAX)==1);wire(&d->lower);
}
static unsigned sout(struct local *d)
{
    for(unsigned i=0;i<300 && d->next==d->lower.outputs;++i)spoll(d);
    assert(d->next<d->lower.outputs);unsigned o=d->next++;
    for(unsigned i=0;i<5;++i)spoll(d);return o;
}
static void start_profile(struct local *d,unsigned mtu,const struct bt_sdp_ag_profile *profile)
{
    memset(d,0,sizeof(*d));ready(&d->lower.f,&d->lower.a);
    d->lower.link=begin_connect(&d->lower.f,&d->lower.a,1);
    connected(&d->lower.f,&d->lower.a,d->lower.link,0x23);
    d->lower.wire_cursor=d->lower.f.tn;d->lower.automatic_credits=1;d->allow=1;
    assert(bt_sdp_server_init(&d->server,&d->lower.q,profile,1000,5000,local_policy,d)==1);
    assert(bt_sdp_server_init(&d->server,&d->lower.q,profile,1000,5000,local_policy,d)==-1);
    assert(bt_rfcomm_registry_init(&d->registry,&d->lower.q,route,service_policy,d)==1);
    assert(bt_l2cap_init(&d->lower.q,&d->lower.a,672,1000000,60000000,bt_rfcomm_admit,&d->registry)==1);
    const struct l2cap_conn_req req={SDP_PSM,0x60};signal_in(&d->lower,L2CAP_CONN_REQ,40,&req,sizeof(req));
    unsigned out=sout(d);const struct l2cap_cmd_hdr *c=command_out(&d->lower,out,L2CAP_CONN_RSP);
    const uint8_t expected[]={0x40,0,0x60,0,0,0,0,0};assert(!memcmp(c+1,expected,8));
    assert(d->server.sessions[0].channel==d->lower.q.channels[0].generation);
    out=sout(d);c=command_out(&d->lower,out,L2CAP_CONF_REQ);
    const uint8_t accepted[]={0x40,0,0,0,0,0};signal_in(&d->lower,L2CAP_CONF_RSP,c->ident,accepted,6);
    uint8_t peer[]={0x40,0,0,0,1,2,(uint8_t)mtu,(uint8_t)(mtu>>8)};
    signal_in(&d->lower,L2CAP_CONF_REQ,41,peer,sizeof(peer));out=sout(d);command_out(&d->lower,out,L2CAP_CONF_RSP);
    assert(d->lower.q.channels[0].state==BT_L2CAP_OPEN);
}
static void start(struct local *d,unsigned mtu)
{const struct bt_sdp_ag_profile p={0x10001,9,0x109,9,7,0,0,0};start_profile(d,mtu,&p);}
static void rx(struct local *d,const uint8_t *p,unsigned n)
{
    uint8_t all[BT_L2CAP_MTU+4];struct l2cap_hdr h={(uint16_t)n,d->lower.q.channels[0].local_cid};
    assert(n<=BT_L2CAP_MTU);memcpy(all,&h,4);memcpy(all+4,p,n);
    for(unsigned at=0;at<n+4;){unsigned size=n+4-at;if(size>23)size=23;
        incoming_fragment(&d->lower,at?1:2,all+at,size);spoll(d);at+=size;}
}
static unsigned get16be(const uint8_t *p){return ((unsigned)p[0]<<8)|p[1];}
static const uint8_t *response(struct local *d,unsigned type,unsigned tid,unsigned *n)
{
    unsigned out=sout(d);struct l2cap_hdr h;memcpy(&h,d->lower.output[out],4);
    const uint8_t *p=d->lower.output[out]+4;
    assert(h.cid==0x60 && p[0]==type && get16be(p+1)==tid && get16be(p+3)==h.len-5u);
    assert(h.len<=d->lower.q.channels[0].remote_mtu);*n=h.len;return p;
}
static void request_send(struct local *d,unsigned type,unsigned tid,const uint8_t *body,unsigned n)
{
    uint8_t p[BT_L2CAP_MTU]={(uint8_t)type,(uint8_t)(tid>>8),(uint8_t)tid,(uint8_t)(n>>8),(uint8_t)n};
    memcpy(p+5,body,n);rx(d,p,n+5);
}
static void search_and_attributes(void)
{
    struct local d;start(&d,672);unsigned n;
    const uint8_t search[]={SDP_SEQ8,3,SDP_UUID16,0x11,0x1f,0,1,0};
    request_send(&d,SDP_SVC_SEARCH_REQ,0x9876,search,sizeof(search));
    const uint8_t *p=response(&d,SDP_SVC_SEARCH_RSP,0x9876,&n);
    const uint8_t answer[]={0,1,0,1,0,1,0,1,0};assert(n==14&&!memcmp(p+5,answer,9));
    const uint8_t ag[]={0x35,0x44,9,0,0,10,0,1,0,1,9,0,1,0x35,6,0x19,0x11,0x1f,0x19,0x12,3,
        9,0,4,0x35,12,0x35,3,0x19,1,0,0x35,5,0x19,0,3,8,7,
        9,0,5,0x35,3,0x19,0x10,2,9,0,9,0x35,8,0x35,6,0x19,0x11,0x1e,9,1,9,
        9,3,1,8,0,9,3,0x11,9,0,9};
    /* Independent canonical service bytes, including Network and feature bitmap. */
    const uint8_t attrs[]={0,1,0,1,0xff,0xff,SDP_SEQ8,5,SDP_UINT32,0,0,0xff,0xff,0};
    request_send(&d,SDP_SVC_ATTR_REQ,2,attrs,sizeof(attrs));p=response(&d,SDP_SVC_ATTR_RSP,2,&n);
    assert(get16be(p+5)==sizeof(ag) && p[7+sizeof(ag)]==0 && !memcmp(p+7,ag,sizeof(ag)));
    const uint8_t server[]={0,0,0,0,0,64,SDP_SEQ8,5,SDP_UINT32,0,0,0xff,0xff,0};
    request_send(&d,SDP_SVC_ATTR_REQ,3,server,sizeof(server));p=response(&d,SDP_SVC_ATTR_RSP,3,&n);
    const uint8_t expected[]={0x35,24,9,0,0,10,0,0,0,0,9,0,1,0x35,3,0x19,0x10,0,9,2,0,0x35,3,9,1,0};
    assert(get16be(p+5)==sizeof(expected)&&!memcmp(p+7,expected,sizeof(expected)));
    const uint8_t combined[]={SDP_SEQ8,6,SDP_UUID16,0x11,0x1f,SDP_UUID16,0x12,3,
        0,7,SDP_SEQ8,5,SDP_UINT32,0,0,0xff,0xff,0};
    uint8_t req[80],assembled[256];memcpy(req,combined,sizeof(combined));unsigned qn=sizeof(combined),used=0,tid=4,loops=0;
    do {
        request_send(&d,SDP_SVC_SEARCH_ATTR_REQ,tid,req,qn);p=response(&d,SDP_SVC_SEARCH_ATTR_RSP,tid++,&n);
        unsigned count=get16be(p+5);assert(count>=2&&count<=7&&used+count<=sizeof(assembled));
        memcpy(assembled+used,p+7,count);used+=count;unsigned cn=p[7+count];
        if(!cn)break;assert(cn==12&&++loops<20);
        qn=sizeof(combined)+cn;req[sizeof(combined)-1]=(uint8_t)cn;memcpy(req+sizeof(combined),p+8+count,cn);
    } while(1);
    assert(used==sizeof(ag)+2 && assembled[0]==0x35 && assembled[1]==sizeof(ag) && !memcmp(assembled+2,ag,sizeof(ag)));
    assert(bt_sdp_server_admit(&d.server,d.lower.link,1,1)==0);
    assert(bt_rfcomm_admit(&d.registry,d.lower.link,3,1)==0);
    puts("SDP server search, mandatory records and exact continued combined attributes pass");
}
static void errors_and_bounds(void)
{
    struct local d;start(&d,48);unsigned n;const uint8_t *p;
    uint8_t req[]={SDP_SEQ8,3,SDP_UUID16,0x11,0x1f,0,255,SDP_SEQ8,5,SDP_UINT32,0,0,0xff,0xff,0};
    request_send(&d,SDP_SVC_SEARCH_ATTR_REQ,1,req,sizeof(req));p=response(&d,SDP_SVC_SEARCH_ATTR_RSP,1,&n);
    unsigned bytes=get16be(p+5);assert(n==48 && p[7+bytes]==12);
    uint8_t continuation[64];memcpy(continuation,req,sizeof(req));continuation[sizeof(req)-1]=12;
    memcpy(continuation+sizeof(req),p+8+bytes,12);continuation[sizeof(req)+11]^=1;
    request_send(&d,SDP_SVC_SEARCH_ATTR_REQ,2,continuation,sizeof(req)+12);p=response(&d,SDP_ERROR_RSP,2,&n);assert(get16be(p+5)==5);
    continuation[sizeof(req)+11]^=1;continuation[6]^=1;
    request_send(&d,SDP_SVC_SEARCH_ATTR_REQ,3,continuation,sizeof(req)+12);p=response(&d,SDP_ERROR_RSP,3,&n);assert(get16be(p+5)==5);
    uint8_t badhandle[]={0,2,0,0,0,64,SDP_SEQ8,3,SDP_UINT16,0,0,0};
    request_send(&d,SDP_SVC_ATTR_REQ,4,badhandle,sizeof(badhandle));p=response(&d,SDP_ERROR_RSP,4,&n);assert(get16be(p+5)==2);
    uint8_t badrange[]={0,1,0,1,0,64,SDP_SEQ8,5,SDP_UINT32,0,9,0,1,0};
    request_send(&d,SDP_SVC_ATTR_REQ,5,badrange,sizeof(badrange));p=response(&d,SDP_ERROR_RSP,5,&n);assert(get16be(p+5)==3);
    const uint8_t unknown[]={SDP_SEQ8,17,SDP_UUID128,0,0,0x11,0x1f,0,0,0x10,0,0x80,0,0,0x80,0x5f,0x9b,0x34,0xfa,0,1,0};
    request_send(&d,SDP_SVC_SEARCH_REQ,6,unknown,sizeof(unknown));p=response(&d,SDP_SVC_SEARCH_RSP,6,&n);assert(get16be(p+5)==0&&n==10);
    uint8_t equivalent[sizeof(unknown)];memcpy(equivalent,unknown,sizeof(unknown));equivalent[18]=0xfb;
    request_send(&d,SDP_SVC_SEARCH_REQ,7,equivalent,sizeof(equivalent));p=response(&d,SDP_SVC_SEARCH_RSP,7,&n);assert(get16be(p+5)==1);
    const uint8_t malformed[]={2,0,8,0,10,0};rx(&d,malformed,sizeof(malformed));p=response(&d,SDP_ERROR_RSP,8,&n);assert(get16be(p+5)==4);
    puts("SDP server MTU, UUID representation and bounded protocol errors pass");
}
static void continuation_lifetimes(void)
{
    struct local d;start(&d,48);unsigned n;
    const uint8_t req[]={SDP_SEQ8,3,SDP_UUID16,0x11,0x1f,0,255,SDP_SEQ8,5,SDP_UINT32,0,0,0xff,0xff,0};
    request_send(&d,SDP_SVC_SEARCH_ATTR_REQ,1,req,sizeof(req));
    const uint8_t *p=response(&d,SDP_SVC_SEARCH_ATTR_RSP,1,&n);unsigned count=get16be(p+5);
    uint8_t old[sizeof(req)+12];memcpy(old,req,sizeof(req));old[sizeof(req)-1]=12;
    memcpy(old+sizeof(req),p+8+count,12);
    request_send(&d,SDP_SVC_SEARCH_ATTR_REQ,2,req,sizeof(req));p=response(&d,SDP_SVC_SEARCH_ATTR_RSP,2,&n);
    request_send(&d,SDP_SVC_SEARCH_ATTR_REQ,3,old,sizeof(old));p=response(&d,SDP_ERROR_RSP,3,&n);assert(get16be(p+5)==5);
    request_send(&d,SDP_SVC_SEARCH_ATTR_REQ,4,req,sizeof(req));p=response(&d,SDP_SVC_SEARCH_ATTR_RSP,4,&n);
    uint8_t expired[sizeof(old)];memcpy(expired,old,sizeof(old));count=get16be(p+5);memcpy(expired+sizeof(req),p+8+count,12);
    d.lower.f.now=d.server.sessions[0].query_deadline;
    request_send(&d,SDP_SVC_SEARCH_ATTR_REQ,5,expired,sizeof(expired));p=response(&d,SDP_ERROR_RSP,5,&n);assert(get16be(p+5)==5);
    uint64_t previous=d.server.sessions[0].channel;assert(bt_sdp_server_close(&d.server,previous,UINT64_MAX)==1);
    unsigned out=sout(&d);const struct l2cap_cmd_hdr *cmd=command_out(&d.lower,out,L2CAP_DISCONN_REQ);
    uint8_t reply[4];memcpy(reply,cmd+1,4);signal_in(&d.lower,L2CAP_DISCONN_RSP,cmd->ident,reply,4);
    for(unsigned i=0;i<15;++i)spoll(&d);
    assert(bt_sdp_server_release_channel(&d.server,previous)==1);
    unsigned cid=d.lower.q.next_cid;struct l2cap_conn_req conn={SDP_PSM,0x60};
    signal_in(&d.lower,L2CAP_CONN_REQ,44,&conn,4);out=sout(&d);command_out(&d.lower,out,L2CAP_CONN_RSP);
    out=sout(&d);cmd=command_out(&d.lower,out,L2CAP_CONF_REQ);
    uint8_t accepted[]={(uint8_t)cid,(uint8_t)(cid>>8),0,0,0,0};signal_in(&d.lower,L2CAP_CONF_RSP,cmd->ident,accepted,6);
    uint8_t peer[]={(uint8_t)cid,(uint8_t)(cid>>8),0,0,1,2,48,0};signal_in(&d.lower,L2CAP_CONF_REQ,45,peer,8);
    out=sout(&d);command_out(&d.lower,out,L2CAP_CONF_RSP);assert(d.server.sessions[0].channel!=previous);
    request_send(&d,SDP_SVC_SEARCH_ATTR_REQ,6,req,sizeof(req));response(&d,SDP_SVC_SEARCH_ATTR_RSP,6,&n);
    request_send(&d,SDP_SVC_SEARCH_ATTR_REQ,7,old,sizeof(old));p=response(&d,SDP_ERROR_RSP,7,&n);assert(get16be(p+5)==5);
    puts("SDP continuation rejects new-query replay, expiry and other channel generations");
}
static void ownership(void)
{
    struct local d;start(&d,672);unsigned n;
    d.lower.automatic_credits=0;
    const uint8_t search[]={SDP_SEQ8,3,SDP_UUID16,0x11,0x1f,0,1,0};
    request_send(&d,SDP_SVC_SEARCH_REQ,1,search,sizeof(search));response(&d,SDP_SVC_SEARCH_RSP,1,&n);
    assert(d.server.sessions[0].tx_serial);
    request_send(&d,SDP_SVC_SEARCH_REQ,2,search,sizeof(search));for(unsigned i=0;i<10;++i)spoll(&d);
    assert(d.next==d.lower.outputs && bt_l2cap_receive(&d.lower.q));
    d.lower.f.now=d.server.sessions[0].deadline;spoll(&d);
    assert(d.server.sessions[0].error==BT_SDP_SERVER_EXPIRED && d.server.sessions[0].tx_serial);
    uint64_t serial=d.server.sessions[0].tx_serial;for(unsigned i=0;i<4;++i)spoll(&d);
    assert(d.server.sessions[0].tx_serial==serial&&bt_l2cap_receive(&d.lower.q));
    assert(bt_sdp_server_release_channel(&d.server,d.server.sessions[0].channel)==-1);
    puts("SDP server credit backpressure and deadline retain owned requests and TX");
}
static void close_pending_tx(void)
{
    struct local d;start(&d,672);unsigned n;uint64_t identity=d.server.sessions[0].channel;
    d.lower.automatic_credits=0;
    const uint8_t search[]={SDP_SEQ8,3,SDP_UUID16,0x11,0x1f,0,1,0};
    request_send(&d,SDP_SVC_SEARCH_REQ,1,search,sizeof(search));response(&d,SDP_SVC_SEARCH_RSP,1,&n);
    assert(d.server.sessions[0].tx_serial);
    assert(bt_sdp_server_close(&d.server,identity,UINT64_MAX)==1);
    for(unsigned i=0;i<8;++i)spoll(&d);
    assert(d.lower.q.channels[0].state==BT_L2CAP_OPEN && !d.server.sessions[0].close_started);
    const uint8_t credit[]={1,0x23,0,1,0};event(&d.lower.f,HCI_EV_NUM_COMP_PKTS,credit,sizeof(credit));
    d.lower.automatic_credits=1;unsigned out=sout(&d);
    const struct l2cap_cmd_hdr *c=command_out(&d.lower,out,L2CAP_DISCONN_REQ);
    assert(!d.server.sessions[0].tx_serial && d.server.sessions[0].error==BT_SDP_SERVER_OK);
    uint8_t reply[4];memcpy(reply,c+1,4);signal_in(&d.lower,L2CAP_DISCONN_RSP,c->ident,reply,4);
    for(unsigned i=0;i<15;++i)spoll(&d);
    assert(bt_sdp_server_release_channel(&d.server,identity)==1);
    puts("SDP server close waits for actual controller-credit retirement");
}
static void close_and_short(void)
{
    struct local d;start(&d,672);uint64_t identity=d.server.sessions[0].channel;
    const uint8_t short_pdu[]={2,0,1};rx(&d,short_pdu,sizeof(short_pdu));spoll(&d);
    assert(d.server.sessions[0].error==BT_SDP_SERVER_SHORT&&d.server.sessions[0].held_rx);
    assert(bt_sdp_server_close(&d.server,identity,UINT64_MAX)==1);
    unsigned out=sout(&d);const struct l2cap_cmd_hdr *c=command_out(&d.lower,out,L2CAP_DISCONN_REQ);
    uint8_t reply[4];memcpy(reply,c+1,4);signal_in(&d.lower,L2CAP_DISCONN_RSP,c->ident,reply,4);
    for(unsigned i=0;i<15;++i)spoll(&d);
    assert(bt_sdp_server_release_channel(&d.server,identity)==1&&!d.server.sessions[0].channel);
    puts("SDP server short identity retains raw input until confirmed close");
}
/* The optional A2DP sink record, encoded independently from A2DP1.3 and
 * AVDTP1.3: AudioSink class, L2CAP PSM0x19, AVDTP0x0103, A2DP0x0103. */
static void sink_record(void)
{
    struct local d;unsigned n;const uint8_t *p;
    const struct bt_sdp_ag_profile bad[]={{0x10001,9,0x109,9,7,0,0x10001,1},{0x10001,9,0x109,9,7,0,0x20000,0},
        {0x10001,9,0x109,9,7,0,0x20000,16},{0x10001,9,0x109,9,7,0,0,1},{0x10001,9,0x109,9,7,0,0xffff,1}};
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);++i){struct bt_sdp_server s={0};struct bt_l2cap l={0};
        assert(bt_sdp_server_init(&s,&l,&bad[i],1000,5000,local_policy,&d)==-1);}
    const struct bt_sdp_ag_profile profile={0x10001,9,0x109,9,7,0,0x10002,4};start_profile(&d,672,&profile);
    const uint8_t sink[]={SDP_SEQ8,3,SDP_UUID16,0x11,0x0b,0,4,0};
    request_send(&d,SDP_SVC_SEARCH_REQ,1,sink,sizeof(sink));p=response(&d,SDP_SVC_SEARCH_RSP,1,&n);
    const uint8_t one[]={0,1,0,1,0,1,0,2,0};assert(n==14 && !memcmp(p+5,one,9));
    const uint8_t browse[]={SDP_SEQ8,3,SDP_UUID16,0x10,0x02,0,4,0};
    request_send(&d,SDP_SVC_SEARCH_REQ,2,browse,sizeof(browse));p=response(&d,SDP_SVC_SEARCH_RSP,2,&n);
    const uint8_t two[]={0,2,0,2,0,1,0,1,0,1,0,2,0};assert(n==18 && !memcmp(p+5,two,13));
    const uint8_t record[]={0x35,0x40,9,0,0,10,0,1,0,2,9,0,1,0x35,3,0x19,0x11,0x0b,
        9,0,4,0x35,16,0x35,6,0x19,1,0,9,0,0x19,0x35,6,0x19,0,0x19,9,1,3,
        9,0,5,0x35,3,0x19,0x10,2,9,0,9,0x35,8,0x35,6,0x19,0x11,0x0d,9,1,3,9,3,0x11,9,0,4};
    const uint8_t attrs[]={0,1,0,2,0xff,0xff,SDP_SEQ8,5,SDP_UINT32,0,0,0xff,0xff,0};
    request_send(&d,SDP_SVC_ATTR_REQ,3,attrs,sizeof(attrs));p=response(&d,SDP_SVC_ATTR_RSP,3,&n);
    assert(get16be(p+5)==sizeof(record) && p[7+sizeof(record)]==0 && !memcmp(p+7,record,sizeof(record)));
    const uint8_t combined[]={SDP_SEQ8,6,SDP_UUID16,0x11,0x0b,SDP_UUID16,0,0x19,1,0,SDP_SEQ8,5,SDP_UINT32,0,0,0xff,0xff,0};
    request_send(&d,SDP_SVC_SEARCH_ATTR_REQ,4,combined,sizeof(combined));p=response(&d,SDP_SVC_SEARCH_ATTR_RSP,4,&n);
    assert(get16be(p+5)==sizeof(record)+2 && p[7]==0x35 && p[8]==sizeof(record) && !memcmp(p+9,record,sizeof(record)));
    const uint8_t missing[]={0,1,0,3,0,64,SDP_SEQ8,3,SDP_UINT16,0,0,0};
    request_send(&d,SDP_SVC_ATTR_REQ,5,missing,sizeof(missing));p=response(&d,SDP_ERROR_RSP,5,&n);assert(get16be(p+5)==2);
    struct local plain;start(&plain,672);
    request_send(&plain,SDP_SVC_SEARCH_REQ,6,sink,sizeof(sink));p=response(&plain,SDP_SVC_SEARCH_RSP,6,&n);assert(get16be(p+5)==0);
    request_send(&plain,SDP_SVC_ATTR_REQ,7,attrs,sizeof(attrs));p=response(&plain,SDP_ERROR_RSP,7,&n);assert(get16be(p+5)==2);
    puts("SDP server optional A2DP sink record, searches and absence pass");
}
int main(void)
{
    assert(sizeof(sdp_pdu_hdr_t)==5 && SDP_SVC_SEARCH_REQ==2 && SDP_SVC_ATTR_REQ==4);
    search_and_attributes();errors_and_bounds();sink_record();continuation_lifetimes();ownership();close_pending_tx();close_and_short();return 0;
}
