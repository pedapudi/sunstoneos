/* SPDX-License-Identifier: Apache-2.0 */
#include "rfcomm-server-fixture.h"
#include "bt_hfp_ag.h"
#include "bt_sdp_server.h"
struct gateway {struct serial_fixture rf;struct bt_sdp_server sdp;struct bt_hfp_ag ag;};
static int ag_policy(void *p,uint64_t link,uint16_t psm,unsigned incoming)
{struct gateway *g=p;(void)incoming;assert(link==g->rf.d.lower.link);return g->rf.allowed && (psm==1 || psm==3);}
static int ag_route(void *p,uint64_t link,uint16_t psm,unsigned incoming)
{return bt_sdp_server_admit(&((struct gateway *)p)->sdp,link,psm,incoming);}
static int ag_service(void *p,uint64_t link,uint16_t uuid,uint8_t server)
{struct gateway *g=p;assert(link==g->rf.d.lower.link && uuid==0x111f && server==7);return g->rf.allowed;}
static void lower_poll(struct gateway *g)
{struct simulation *s=&g->rf.d.lower;s->f.room=1024;++s->f.now;assert(bt_sdp_server_step(&g->sdp,64,UINT64_MAX)==1);wire(s);}
static unsigned lower_output(struct gateway *g)
{for(unsigned i=0;i<300 && g->rf.d.next_output==g->rf.d.lower.outputs;++i)lower_poll(g);
 assert(g->rf.d.next_output<g->rf.d.lower.outputs);unsigned n=g->rf.d.next_output++;for(unsigned i=0;i<4;++i)lower_poll(g);return n;}
static void incoming_start(struct gateway *g)
{
    memset(g,0,sizeof(*g));g->rf.allowed=1;struct simulation *s=&g->rf.d.lower;ready(&s->f,&s->a);
    assert(bt_acl_incoming(&s->a,2000)==1);const uint8_t address[]={1,2,3,4,5,6};
    assert(bt_acl_listen(&s->a,address,10000,&s->link)==1 && s->link==1);
    struct hci_ev_conn_request req={{{1,2,3,4,5,6}},{4,4,0x24},1};
    event(&s->f,HCI_EV_CONN_REQUEST,&req,sizeof(req));astep(&s->a);size_t before=s->f.tn;astep(&s->a);
    const uint8_t accept[]={1,9,4,7,1,2,3,4,5,6,1};assert(s->f.tn==before+sizeof(accept) && !memcmp(s->f.tx+before,accept,sizeof(accept)));
    cs(&s->f,HCI_OP_ACCEPT_CONN_REQ,1,0);astep(&s->a);assert(s->a.links[0].state==BT_ACL_ACCEPTING);
    struct hci_ev_conn_complete conn={0,0x23,{{1,2,3,4,5,6}},1,0};event(&s->f,HCI_EV_CONN_COMPLETE,&conn,sizeof(conn));astep(&s->a);
    assert(s->a.links[0].state==BT_ACL_CONNECTED && s->a.links[0].incoming);
    s->wire_cursor=s->f.tn;s->automatic_credits=1;
    const struct bt_sdp_ag_profile profile={0x10001,BT_HFP_AG_FEATURES,0x109,0,7,0};
    assert(bt_sdp_server_init(&g->sdp,&s->q,&profile,1000000,5000000,ag_policy,g)==1);
    assert(bt_rfcomm_registry_init(&g->rf.registry,&s->q,ag_route,ag_service,g)==1);
    assert(bt_l2cap_init(&s->q,&s->a,672,1000000,60000000,bt_rfcomm_admit,&g->rf.registry)==1);
    assert(bt_rfcomm_listen(&g->rf.client,&g->rf.registry,1,0x111f,7,160,100000000)==1);
    const struct l2cap_conn_req sdpreq={1,0x60};signal_in(s,L2CAP_CONN_REQ,40,&sdpreq,4);
    command_out(s,lower_output(g),L2CAP_CONN_RSP);
    const struct l2cap_cmd_hdr *c=command_out(s,lower_output(g),L2CAP_CONF_REQ);
    const uint8_t accepted[]={0x40,0,0,0,0,0};signal_in(s,L2CAP_CONF_RSP,c->ident,accepted,6);
    const uint8_t mtu[]={0x40,0,0,0,1,2,0xa0,2};signal_in(s,L2CAP_CONF_REQ,41,mtu,8);
    command_out(s,lower_output(g),L2CAP_CONF_RSP);
    /* Query the actual local AG record before opening its advertised service. */
    const uint8_t query[]={4,0,1,0,14,0,1,0,1,0xff,0xff,0x35,5,0x0a,0,0,0xff,0xff,0};
    incoming_pdu(s,0x40,query,sizeof(query),0);unsigned out=lower_output(g);const uint8_t *reply=s->output[out]+4;
    const uint8_t record[]={0x35,0x44,9,0,0,10,0,1,0,1,9,0,1,0x35,6,0x19,0x11,0x1f,0x19,0x12,3,
        9,0,4,0x35,12,0x35,3,0x19,1,0,0x35,5,0x19,0,3,8,7,
        9,0,5,0x35,3,0x19,0x10,2,9,0,9,0x35,8,0x35,6,0x19,0x11,0x1e,9,1,9,
        9,3,1,8,0,9,3,0x11,9,0,0};
    assert(reply[0]==5 && reply[1]==0 && reply[2]==1 && reply[5]==0 && reply[6]==sizeof(record));
    assert(!memcmp(reply+7,record,sizeof(record)) && reply[7+sizeof(record)]==0);
    server_l2(&g->rf,64);rin(&g->rf,0,RFCOMM_SABM|0x10,1,0,0,-1);
    expected_frame(&g->rf,rout(&g->rf),0,RFCOMM_UA|0x10,1,0,0,-1);
    const uint8_t pn[]={14,0xf0,7,0,160,0,0,7};server_mcc(&g->rf,RFCOMM_PN,1,pn,8);
    const uint8_t pnr[]={14,0xe0,7,0,58,0,0,4};server_expect(&g->rf,RFCOMM_PN,0,pnr,8);
    rin(&g->rf,14,RFCOMM_SABM|0x10,1,0,0,-1);expected_frame(&g->rf,rout(&g->rf),14,RFCOMM_UA|0x10,1,0,0,-1);
    const uint8_t msc[]={59,0x8d};server_expect(&g->rf,RFCOMM_MSC,1,msc,2);
    server_mcc(&g->rf,RFCOMM_MSC,0,msc,2);server_mcc(&g->rf,RFCOMM_MSC,1,msc,2);
    server_expect(&g->rf,RFCOMM_MSC,0,msc,2);rpoll(&g->rf);assert(g->rf.client.phase==BT_RFCOMM_OPEN);
    rin(&g->rf,14,RFCOMM_UIH|0x10,1,0,0,100);
    assert(s->q.channels[0].state==BT_L2CAP_OPEN && g->sdp.sessions[0].channel==1);
}
static void ag_init(struct gateway *g)
{const uint8_t actual[]={0,0,0,0,0,0,4};assert(bt_hfp_ag_init(&g->ag,&g->rf.client,actual,100000000,1000000)==1);}
static int ag_poll(struct gateway *g)
{
    struct simulation *s=&g->rf.d.lower;s->f.room=1024;++s->f.now;
    int r=bt_hfp_ag_step(&g->ag,64,32,UINT64_MAX);wire(s);
    if(r>=0)lower_poll(g);return r;
}
static void command_bytes(struct gateway *g,const char *p)
{rin(&g->rf,14,RFCOMM_UIH,1,(const uint8_t *)p,(unsigned)strlen(p),-1);}
static void response_bytes(struct gateway *g,const char *expected)
{
    struct simulation *s=&g->rf.d.lower;unsigned copied=0;size_t length=strlen(expected);
    for(unsigned tries=0;tries<2000 && copied<length;++tries){assert(ag_poll(g)==1);
        while(g->rf.d.next_output<s->outputs){unsigned out=g->rf.d.next_output++;const uint8_t *p=s->output[out]+4;
            assert((p[0]>>2)==14 && (p[1]&0xef)==RFCOMM_UIH && !(p[0]&2));
            unsigned n=p[2]>>1;assert(p[2]&1);
            if(p[1]&0x10){assert(!n);continue;}
            assert(n && copied+n<=length && !memcmp(expected+copied,p+3,n));copied+=n;}}
    assert(copied==length);
    for(unsigned i=0;i<8;++i)assert(ag_poll(g)==1);
}
static void establish(struct gateway *g)
{
    incoming_start(g);
    /* The legitimate first command may already be owned when AG binds. */
    command_bytes(g,"AT+BR");ag_init(g);assert(ag_poll(g)==1 && g->ag.line_length==5);
    command_bytes(g,"SF=4294967295\r");response_bytes(g,"\r\n+BRSF: 0\r\n\r\nOK\r\n");
    assert(g->ag.hf_features==UINT32_MAX && g->ag.stage==1 && g->ag.phase==BT_HFP_AG_INITIALIZING);
    command_bytes(g,"AT+CIND=?\rAT+CIND?\r");
    response_bytes(g,"\r\n+CIND: (\"service\",(0,1)),(\"call\",(0,1)),(\"callsetup\",(0-3)),(\"callheld\",(0-2)),(\"signal\",(0-5)),(\"roam\",(0,1)),(\"battchg\",(0-5))\r\n\r\nOK\r\n\r\n+CIND: 0,0,0,0,0,0,4\r\n\r\nOK\r\n");
    command_bytes(g,"AT+CMER=3,,,1\r");response_bytes(g,"\r\nOK\r\n");assert(g->ag.phase==BT_HFP_AG_READY && g->ag.reporting);
}
static void commands_and_indicators(void)
{
    struct gateway g;establish(&g);
    command_bytes(&g,"AT+CMER=,,,0,\r");response_bytes(&g,"\r\nOK\r\n");assert(!g.ag.reporting && g.ag.cmer_mode==3);
    command_bytes(&g,"AT+CMER=,,,1\r");response_bytes(&g,"\r\nOK\r\n");assert(g.ag.reporting);
    command_bytes(&g,"AT+BIA=0,0,0,0,,0,,1,0,1\r");response_bytes(&g,"\r\nOK\r\n");assert(g.ag.active==94);
    const uint8_t actual[]={1,1,2,0,4,1,3};assert(bt_hfp_ag_indicators(&g.ag,actual)==1);
    response_bytes(&g,"\r\n+CIEV: 2,1\r\n\r\n+CIEV: 3,2\r\n\r\n+CIEV: 5,4\r\n\r\n+CIEV: 7,3\r\n");
    command_bytes(&g,"AT+CIND?\r");response_bytes(&g,"\r\n+CIND: 1,1,2,0,4,1,3\r\n\r\nOK\r\n");
    command_bytes(&g,"AT+VGM=15\rAT+VGS=0\r");response_bytes(&g,"\r\nOK\r\n\r\nOK\r\n");assert(g.ag.gain_valid==3 && g.ag.microphone_gain==15 && !g.ag.speaker_gain);
    command_bytes(&g,"AT+BVRA=1\rAT+BAC=1,2\rAT+VGM=16\r");response_bytes(&g,"\r\nERROR\r\n\r\nERROR\r\n\r\nERROR\r\n");
    assert(g.ag.phase==BT_HFP_AG_READY && g.sdp.profile.brsf_features==BT_HFP_AG_FEATURES);
    uint8_t invalid[]={0,0,0,0,6,0,4};struct bt_hfp_ag before=g.ag;
    assert(bt_hfp_ag_indicators(&g.ag,invalid)==-1 && !memcmp(&before,&g.ag,sizeof(before)));
    assert(bt_hfp_ag_indicators(&g.ag,g.ag.indicators)==-1);
    puts("AG command, omitted CMER, indicator and gain assertions passed");
}
static void close_success(void)
{
    struct gateway g;establish(&g);
    /* Account for every credit-only frame before the separate close reply. */
    while(g.rf.d.next_output<g.rf.d.lower.outputs){unsigned out=g.rf.d.next_output++;
        const uint8_t *p=g.rf.d.lower.output[out]+4;
        expected_frame(&g.rf,out,14,RFCOMM_UIH|0x10,0,0,0,p[3]);}
    rin(&g.rf,0,RFCOMM_DISC|0x10,1,0,0,-1);
    expected_frame(&g.rf,rout(&g.rf),0,RFCOMM_UA|0x10,1,0,0,-1);l2close(&g.rf);
    assert(bt_hfp_ag_step(&g.ag,64,32,UINT64_MAX)==0 && g.ag.phase==BT_HFP_AG_CLOSED);
    assert(bt_hfp_ag_release(&g.ag)==1 && bt_rfcomm_release(&g.rf.client)==1);
    assert(g.sdp.sessions[0].channel==1 && g.rf.d.lower.q.channels[0].state==BT_L2CAP_OPEN);
}

static void backpressure_timeout_and_close(void)
{
    struct gateway g;establish(&g);g.rf.d.lower.automatic_credits=0;
    command_bytes(&g,"AT+VGM=4\r");assert(ag_poll(&g)==1);uint64_t tx=g.ag.transmit;assert(tx);
    /* Close retains the accepted response until actual controller retirement. */
    for(unsigned i=0;i<8;++i)assert(ag_poll(&g)==1);
    assert(g.ag.transmit==tx && bt_hfp_ag_close(&g.ag,g.rf.d.lower.f.now+100)==1);
    assert(bt_hfp_ag_step(&g.ag,64,32,UINT64_MAX)==1 && g.ag.transmit==tx && bt_hfp_ag_release(&g.ag)==-1);
    g.rf.d.lower.f.now=g.rf.client.close_deadline;
    assert(bt_hfp_ag_step(&g.ag,64,32,UINT64_MAX)==-1 && g.ag.phase==BT_HFP_AG_FAULT && g.ag.transmit==tx);
    /* A fresh session stalls CMER response before any UART acceptance. */
    incoming_start(&g);ag_init(&g);command_bytes(&g,"AT+BRSF=0\r");response_bytes(&g,"\r\n+BRSF: 0\r\n\r\nOK\r\n");
    command_bytes(&g,"AT+CIND=?\rAT+CIND?\r");
    response_bytes(&g,"\r\n+CIND: (\"service\",(0,1)),(\"call\",(0,1)),(\"callsetup\",(0-3)),(\"callheld\",(0-2)),(\"signal\",(0-5)),(\"roam\",(0,1)),(\"battchg\",(0-5))\r\n\r\nOK\r\n\r\n+CIND: 0,0,0,0,0,0,4\r\n\r\nOK\r\n");
    command_bytes(&g,"AT+CMER=3,0,0,1\r");g.rf.d.lower.f.room=0;
    assert(bt_hfp_ag_step(&g.ag,64,32,UINT64_MAX)==1 && g.ag.phase==BT_HFP_AG_INITIALIZING);
    g.rf.d.lower.f.now=g.ag.response_deadline;
    assert(bt_hfp_ag_step(&g.ag,64,32,UINT64_MAX)==-1 && g.ag.error==BT_HFP_AG_EXPIRED && g.ag.transmit);
}
static void malformed_and_security(void)
{
    struct gateway g;incoming_start(&g);ag_init(&g);
    command_bytes(&g,"AT+CIND?\rAT+BRSF=4294967296\r");response_bytes(&g,"\r\nERROR\r\n\r\nERROR\r\n");
    char line[59];memset(line,'A',58);line[58]=0;
    for(unsigned i=0;i<3;++i){command_bytes(&g,line);for(unsigned j=0;j<3;++j)assert(ag_poll(&g)==1);}
    assert(g.ag.discard);command_bytes(&g,"\rAT+BRSF=0\r");response_bytes(&g,"\r\nERROR\r\n\r\n+BRSF: 0\r\n\r\nOK\r\n");
    command_bytes(&g,"AT+C");assert(ag_poll(&g)==1 && g.ag.line_deadline);g.rf.d.lower.f.now=g.ag.line_deadline;
    assert(bt_hfp_ag_step(&g.ag,64,32,UINT64_MAX)==-1 && g.ag.error==BT_HFP_AG_EXPIRED && g.ag.line_length==4);
    incoming_start(&g);ag_init(&g);g.rf.allowed=0;unsigned accesses=g.rf.d.lower.f.accesses;
    assert(bt_hfp_ag_step(&g.ag,64,32,UINT64_MAX)==-1 && g.ag.error==BT_HFP_AG_LOWER && g.rf.d.lower.f.accesses==accesses);
}
int main(int argc,char **argv)
{
    if(argc!=2 || strcmp(argv[1],"--remaining"))commands_and_indicators();
    close_success();backpressure_timeout_and_close();malformed_and_security();
    puts("AG SLC, truthful SDP features, bounded AT streams/indicators, response acceptance and retained faults pass over incoming HCI/ACL/SDP/RFCOMM.");return 0;
}
