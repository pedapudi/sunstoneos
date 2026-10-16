/* SPDX-License-Identifier: Apache-2.0 */
/* Existing lower fixtures supply setup helpers; their test mains are not run. */
#include "sdp-fixture.h"
#include "bt_rfcomm.h"
#include "rfcomm-ref.h"
struct serial_fixture {
    struct discovery d;
    struct bt_rfcomm_registry registry;
    struct bt_rfcomm client;
    unsigned allowed, calls;
};
static int service_authority(void *p,uint64_t link,uint16_t uuid,uint8_t server)
{
    struct serial_fixture *f=p;++f->calls;
    assert(link==1 && uuid==HANDSFREE_SVCLASS_ID && server==7);return f->allowed ? 1 : -1;
}
static void rpoll(struct serial_fixture *f)
{
    f->d.lower.f.room=1024;++f->d.lower.f.now;
    int result=bt_rfcomm_step(&f->client,64,UINT64_MAX);
    if(result<0)fprintf(stderr,"RFCOMM fault phase=%u error=%u waiting=%u lower=%u\n",f->client.phase,f->client.error,f->client.waiting,f->d.lower.q.error);
    assert(result>=0);wire(&f->d.lower);
}
static unsigned rout(struct serial_fixture *f)
{
    for(unsigned i=0;i<300 && f->d.next_output==f->d.lower.outputs;++i)rpoll(f);
    assert(f->d.next_output<f->d.lower.outputs);unsigned out=f->d.next_output++;
    for(unsigned i=0;i<4;++i)rpoll(f);return out;
}
static uint8_t reference_fcs(const uint8_t *p,unsigned n)
{unsigned f=255;for(unsigned i=0;i<n;++i)f=rfcomm_crc_table[f^p[i]];return (uint8_t)(255-f);}
static unsigned reference_frame(uint8_t *p,unsigned dlci,unsigned ctrl,unsigned cr,const uint8_t *data,unsigned n,int credit)
{
    p[0]=(uint8_t)((dlci<<2)|(cr<<1)|1);p[1]=(uint8_t)ctrl;p[2]=(uint8_t)((n<<1)|(n<128));unsigned at=3;
    if(n>=128)p[at++]=(uint8_t)(n>>7);unsigned covered=(ctrl&0xef)==RFCOMM_UIH ? 2 : at;
    if(credit>=0)p[at++]=(uint8_t)credit;if(n)memcpy(p+at,data,n);at+=n;p[at++]=reference_fcs(p,covered);return at;
}
static unsigned reference_mcc(uint8_t *p,unsigned type,unsigned cr,const uint8_t *data,unsigned n)
{p[0]=(uint8_t)((type<<2)|(cr<<1)|1);p[1]=(uint8_t)((n<<1)|(n<128));unsigned at=2;if(n>=128)p[at++]=(uint8_t)(n>>7);memcpy(p+at,data,n);return at+n;}
static void expected_frame(struct serial_fixture *f,unsigned out,unsigned dlci,unsigned ctrl,unsigned cr,const uint8_t *data,unsigned n,int credit)
{
    uint8_t reference[BT_RFCOMM_MTU+10];unsigned bytes=reference_frame(reference,dlci,ctrl,cr,data,n,credit);
    struct l2cap_hdr h={(uint16_t)bytes,0x61};const uint8_t *p=f->d.lower.output[out];
    assert(f->d.lower.output_length[out]==bytes+4 && !memcmp(p,&h,4) && !memcmp(p+4,reference,bytes));
}
static void rbytes(struct serial_fixture *f,const uint8_t *p,unsigned n,int good)
{
    uint8_t all[BT_L2CAP_MTU+4];struct l2cap_hdr h={(uint16_t)n,0x41};memcpy(all,&h,4);memcpy(all+4,p,n);
    for(unsigned at=0;at<n+4;) {
        unsigned count=n+4-at;if(count>23)count=23;
        incoming_fragment(&f->d.lower,at ? 1 : 2,all+at,count);
        if(good || at+count<n+4)rpoll(f);
        else {++f->d.lower.f.now;f->d.lower.f.room=1024;assert(bt_rfcomm_step(&f->client,64,UINT64_MAX)==-1);wire(&f->d.lower);}
        at+=count;
    }
    if(good)for(unsigned i=0;i<3;++i)rpoll(f);
}
static void rin(struct serial_fixture *f,unsigned dlci,unsigned ctrl,unsigned cr,const uint8_t *p,unsigned n,int credit)
{uint8_t bytes[BT_RFCOMM_MTU+6];unsigned length=reference_frame(bytes,dlci,ctrl,cr,p,n,credit);rbytes(f,bytes,length,1);}
static void minput(struct serial_fixture *f,unsigned type,unsigned command,const uint8_t *p,unsigned n)
{uint8_t bytes[BT_RFCOMM_MTU];unsigned length=reference_mcc(bytes,type,command,p,n);rin(f,0,RFCOMM_UIH,0,bytes,length,-1);}
static void rexpect_mcc(struct serial_fixture *f,unsigned type,unsigned command,const uint8_t *p,unsigned n)
{uint8_t bytes[BT_RFCOMM_MTU];unsigned length=reference_mcc(bytes,type,command,p,n);expected_frame(f,rout(f),0,RFCOMM_UIH,1,bytes,length,-1);}
static void discovery_setup(struct serial_fixture *f)
{
    memset(f,0,sizeof(*f));f->allowed=1;struct simulation *s=&f->d.lower;
    ready(&s->f,&s->a);s->link=begin_connect(&s->f,&s->a,1);connected(&s->f,&s->a,s->link,0x23);
    s->wire_cursor=s->f.tn;s->automatic_credits=1;
    assert(bt_rfcomm_registry_init(&f->registry,&s->q,authorize,service_authority,f)==1);
    assert(bt_l2cap_init(&s->q,&s->a,672,1000000,60000000,bt_rfcomm_admit,&f->registry)==1);
    assert(bt_rfcomm_admit(&f->registry,1,1,1)==1 && bt_rfcomm_admit(&f->registry,1,5,0)==-1);
    assert(bt_sdp_begin(&f->d.client,&s->q,1,HANDSFREE_SVCLASS_ID,100000000)==1);
    unsigned out=doutput(&f->d);const struct l2cap_cmd_hdr *req=command_out(s,out,L2CAP_CONN_REQ);
    struct l2cap_conn_rsp accepted={0x60,0x40,0,0};signal_in(s,L2CAP_CONN_RSP,req->ident,&accepted,8);
    out=doutput(&f->d);req=command_out(s,out,L2CAP_CONF_REQ);
    const uint8_t ok[]={0x40,0,0,0,0,0},peer[]={0x40,0,0,0,1,2,0xa0,2};
    signal_in(s,L2CAP_CONF_RSP,req->ident,ok,6);dpoll(&f->d);signal_in(s,L2CAP_CONF_REQ,40,peer,8);
    command_out(s,doutput(&f->d),L2CAP_CONF_RSP);srequest(&f->d,1,0,0);
    struct encoded e=service();srsp(&f->d,1,e.bytes,e.n,0,0);assert(f->d.client.status==BT_SDP_COMPLETE);
    peer_close(&f->d);assert(f->d.client.phase==BT_SDP_CLOSED);
}
static void rsetup(struct serial_fixture *f)
{
    discovery_setup(f);struct simulation *s=&f->d.lower;
    f->allowed=0;assert(bt_rfcomm_begin(&f->client,&f->registry,&f->d.client,0,0,160,100000000)==-1);
    assert(!f->client.channel && !f->registry.session[0]);f->allowed=1;
    assert(bt_rfcomm_begin(&f->client,&f->registry,&f->d.client,0,1,160,100000000)==-1);
    assert(bt_rfcomm_begin(&f->client,&f->registry,&f->d.client,0,0,160,100000000)==1);
    uint64_t second=0;assert(bt_l2cap_open(&s->q,1,3,100000000,&second)==-1 && !second);
    struct bt_rfcomm other={0};assert(bt_rfcomm_begin(&other,&f->registry,&f->d.client,0,0,160,100000000)==-1);
    assert(bt_rfcomm_admit(&f->registry,1,1,1)==1 && bt_rfcomm_admit(&f->registry,1,3,1)==0);
    unsigned out=rout(f);const struct l2cap_cmd_hdr *req=command_out(s,out,L2CAP_CONN_REQ);
    struct l2cap_conn_req reference={L2CAP_PSM_RFCOMM,0x41};assert(!memcmp(req+1,&reference,4));
    struct l2cap_conn_rsp accepted={0x61,0x41,0,0};signal_in(s,L2CAP_CONN_RSP,req->ident,&accepted,8);
    out=rout(f);req=command_out(s,out,L2CAP_CONF_REQ);
    const uint8_t ok[]={0x41,0,0,0,0,0},peer[]={0x41,0,0,0,1,2,0xa0,2};
    signal_in(s,L2CAP_CONF_RSP,req->ident,ok,6);rpoll(f);signal_in(s,L2CAP_CONF_REQ,41,peer,8);
    command_out(s,rout(f),L2CAP_CONF_RSP);
    expected_frame(f,rout(f),0,RFCOMM_SABM|0x10,1,0,0,-1);
}
static void negotiate(struct serial_fixture *f,unsigned credits)
{
    rin(f,0,RFCOMM_UA|0x10,1,0,0,-1);
    const uint8_t pn[]={14,0xf0,7,0,160,0,0,4};rexpect_mcc(f,RFCOMM_PN,1,pn,8);
    uint8_t answer[]={14,0xef,7,99,160,0,88,(uint8_t)credits};minput(f,RFCOMM_PN,0,answer,8);
    expected_frame(f,rout(f),14,RFCOMM_SABM|0x10,1,0,0,-1);
    rin(f,14,RFCOMM_UA|0x10,1,0,0,-1);
    const uint8_t msc[]={59,0x8d};rexpect_mcc(f,RFCOMM_MSC,1,msc,2);
    minput(f,RFCOMM_MSC,0,msc,2);assert(f->client.phase==BT_RFCOMM_NEGOTIATING);
    minput(f,RFCOMM_MSC,1,msc,2);rexpect_mcc(f,RFCOMM_MSC,0,msc,2);rpoll(f);
    assert(f->client.phase==BT_RFCOMM_OPEN && f->client.tx_credits==credits && f->client.rx_credits==4);
}
static void l2close(struct serial_fixture *f)
{
    struct simulation *s=&f->d.lower;unsigned out=rout(f);
    const struct l2cap_cmd_hdr *req=command_out(s,out,L2CAP_DISCONN_REQ);
    struct l2cap_disconn_req pair={0x61,0x41};assert(!memcmp(req+1,&pair,4));
    signal_in(s,L2CAP_DISCONN_RSP,req->ident,&pair,4);
    for(unsigned i=0;i<30 && f->client.phase!=BT_RFCOMM_CLOSED;++i)rpoll(f);
}
static void data_credit_and_close(void)
{
    struct serial_fixture f;rsetup(&f);negotiate(&f,0);
    uint8_t data[130];for(unsigned i=0;i<sizeof(data);++i)data[i]=(uint8_t)(i^0xa5);
    uint64_t serial;assert(bt_rfcomm_send(&f.client,data,sizeof(data),100000000,&serial)==1);
    unsigned before=f.d.lower.outputs;for(unsigned i=0;i<5;++i)rpoll(&f);assert(f.d.lower.outputs==before);
    rin(&f,14,RFCOMM_UIH|0x10,0,0,0,1);expected_frame(&f,rout(&f),14,RFCOMM_UIH,1,data,sizeof(data),-1);
    const struct bt_rfcomm_buffer *r=bt_rfcomm_result(&f.client);assert(r && r->serial==serial && r->state==BT_RFCOMM_RETIRED);
    assert(bt_rfcomm_release_result(&f.client,serial)==1 && !f.client.tx_credits);
    /* No remote credit is required to replenish our receive capacity. */
    rin(&f,14,RFCOMM_UIH,0,data,sizeof(data),-1);r=bt_rfcomm_receive(&f.client);
    assert(r && r->length==sizeof(data) && !memcmp(r->bytes,data,sizeof(data)) && f.client.rx_credits==3);
    assert(bt_rfcomm_release_receive(&f.client,r->serial)==1);expected_frame(&f,rout(&f),14,RFCOMM_UIH|0x10,1,0,0,1);
    /* Remote serial credits never retire controller-owned copies. */
    rin(&f,14,RFCOMM_UIH|0x10,0,0,0,1);f.d.lower.automatic_credits=0;
    assert(bt_rfcomm_send(&f.client,data,3,100000000,&serial)==1);expected_frame(&f,rout(&f),14,RFCOMM_UIH,1,data,3,-1);
    assert(!bt_rfcomm_result(&f.client) && f.d.lower.a.credits==1);
    rin(&f,14,RFCOMM_UIH|0x10,0,0,0,2);assert(!bt_rfcomm_result(&f.client) && f.d.lower.a.credits==1);
    const uint8_t credit[]={1,0x23,0,1,0};event(&f.d.lower.f,HCI_EV_NUM_COMP_PKTS,credit,5);rpoll(&f);
    assert(bt_rfcomm_result(&f.client)->serial==serial);assert(bt_rfcomm_release_result(&f.client,serial)==1);
    f.d.lower.automatic_credits=1;
    const uint8_t rpn[]={59};minput(&f,RFCOMM_RPN,1,rpn,1);
    const uint8_t rpn_response[]={59,3,3,0,0x11,0x13,0x7f,0x3f};rexpect_mcc(&f,RFCOMM_RPN,0,rpn_response,8);
    const uint8_t test[]={1,2,3};minput(&f,RFCOMM_TEST,1,test,3);rexpect_mcc(&f,RFCOMM_TEST,0,test,3);
    /* A second DLC is refused without disturbing this open data channel. */
    rin(&f,16,RFCOMM_SABM|0x10,0,0,0,-1);expected_frame(&f,rout(&f),16,RFCOMM_DM|0x10,0,0,0,-1);
    assert(f.client.phase==BT_RFCOMM_OPEN);
    assert(bt_rfcomm_send(&f.client,data,1,100000000,&serial)==1);
    f.d.lower.automatic_credits=0;
    assert(bt_rfcomm_close(&f.client,100000000)==1);
    assert(bt_rfcomm_result(&f.client)->serial==serial && bt_rfcomm_result(&f.client)->state==BT_RFCOMM_CANCELLED);
    expected_frame(&f,rout(&f),14,RFCOMM_DISC|0x10,1,0,0,-1);
    rin(&f,14,RFCOMM_UIH,0,data,3,-1);assert(bt_rfcomm_receive(&f.client));
    rin(&f,14,RFCOMM_UA|0x10,1,0,0,-1);
    assert(!f.client.closing_lower && f.d.lower.a.credits==1);
    event(&f.d.lower.f,HCI_EV_NUM_COMP_PKTS,credit,5);f.d.lower.automatic_credits=1;rpoll(&f);
    l2close(&f);assert(f.client.phase==BT_RFCOMM_CLOSED && f.registry.session[0]==&f.client);
    assert(bt_rfcomm_release(&f.client)==0 && bt_rfcomm_release_result(&f.client,serial)==1);
    assert(bt_rfcomm_release(&f.client)==0 && bt_rfcomm_receive(&f.client)->length==3);
    assert(bt_rfcomm_release_receive(&f.client,bt_rfcomm_receive(&f.client)->serial)==1);
    assert(bt_rfcomm_release(&f.client)==1 && !f.registry.session[0] && bt_rfcomm_admit(&f.registry,1,1,1)==1);
}
static void peer_close_and_collision(void)
{
    struct serial_fixture f;rsetup(&f);negotiate(&f,2);
    struct l2cap_conn_req collision={3,0x62};signal_in(&f.d.lower,L2CAP_CONN_REQ,42,&collision,4);
    const struct l2cap_cmd_hdr *reply=command_out(&f.d.lower,rout(&f),L2CAP_CONN_RSP);
    const struct l2cap_conn_rsp *ref=(const void *)(reply+1);assert(ref->result && f.client.phase==BT_RFCOMM_OPEN);
    assert(bt_rfcomm_close(&f.client,f.d.lower.f.now+1000)==1);uint64_t close_bound=f.client.close_deadline;
    f.d.lower.automatic_credits=0;
    rin(&f,0,RFCOMM_DISC|0x10,0,0,0,-1);expected_frame(&f,rout(&f),0,RFCOMM_UA|0x10,0,0,0,-1);
    assert(f.client.close_deadline==close_bound);
    assert(!f.client.closing_lower && f.d.lower.a.credits==1);
    const uint8_t credit[]={1,0x23,0,1,0};event(&f.d.lower.f,HCI_EV_NUM_COMP_PKTS,credit,5);
    f.d.lower.automatic_credits=1;rpoll(&f);
    l2close(&f);assert(f.client.phase==BT_RFCOMM_CLOSED && bt_rfcomm_release(&f.client)==1);
}
static void timeout_security_and_invalid(void)
{
    struct serial_fixture f;rsetup(&f);assert(f.client.request_sent && f.client.timer);
    f.d.lower.f.now=f.client.timer;rpoll(&f);
    assert(f.client.error==BT_RFCOMM_EXPIRED && f.client.phase==BT_RFCOMM_CLOSING);
    l2close(&f);assert(f.client.phase==BT_RFCOMM_CLOSED);
    rsetup(&f);negotiate(&f,1);unsigned accesses=f.d.lower.f.accesses;f.allowed=0;
    assert(bt_rfcomm_step(&f.client,64,UINT64_MAX)==-1 && f.client.error==BT_RFCOMM_SECURITY);
    assert(f.d.lower.f.accesses==accesses && f.registry.session[0]==&f.client && bt_rfcomm_release(&f.client)==-1);
    rsetup(&f);negotiate(&f,1);
    uint8_t bytes[8];unsigned n=reference_frame(bytes,14,RFCOMM_UIH,0,0,0,-1);bytes[n-1]^=1;rbytes(&f,bytes,n,0);
    assert(f.client.error==BT_RFCOMM_PROTOCOL && f.client.held_rx && f.registry.session[0]==&f.client);
    assert(bt_rfcomm_close(&f.client,100000000)==1);l2close(&f);assert(f.client.phase==BT_RFCOMM_CLOSED);
}
static void delayed_observation(void)
{
    for(unsigned early=0;early<2;++early) {
    struct serial_fixture f;rsetup(&f);struct simulation *s=&f.d.lower;
    uint8_t ua[8];unsigned n=reference_frame(ua,0,RFCOMM_UA|0x10,1,0,0,-1);
    incoming_pdu(s,0x41,ua,n,0);rpoll(&f);assert(!f.client.request_sent);
    unsigned out=f.d.next_output;
    for(unsigned i=0;i<20 && s->outputs==out;++i)poll(s);
    assert(s->outputs==out+1);f.d.next_output++;
    uint8_t pn[8]={14,0xf0,7,0,160,0,0,4},m[16];
    unsigned mn=reference_mcc(m,RFCOMM_PN,1,pn,8);expected_frame(&f,out,0,RFCOMM_UIH,1,m,mn,-1);
    const struct bt_l2cap_pdu *p=0;
    for(unsigned i=0;i<BT_L2CAP_PDUS;++i)if(s->q.tx[i].channel==f.client.channel &&
        s->q.tx[i].state!=BT_L2CAP_FREE)p=&s->q.tx[i];
    assert(p && p->accepted==p->length && p->accepted_at && !f.client.request_sent);
    uint64_t accepted=p->accepted_at,watermark=p->accepted_after;
    s->f.now+=1000;
    if(!early){rpoll(&f);assert(f.client.timer==accepted+20000000 && f.client.request_after==watermark);}
    uint8_t answer[]={14,0xe0,7,0,160,0,0,1};mn=reference_mcc(m,RFCOMM_PN,0,answer,8);
    uint8_t bytes[32];n=reference_frame(bytes,0,RFCOMM_UIH,0,m,mn,-1);
    incoming_pdu(s,0x41,bytes,n,0);for(unsigned i=0;i<3;++i)poll(s);
    assert(s->f.hci.transport.next_serial-1>watermark);rpoll(&f);
    expected_frame(&f,rout(&f),14,RFCOMM_SABM|0x10,1,0,0,-1);
    }
}
static void drain_receive_credits(struct serial_fixture *f)
{
    rsetup(f);negotiate(f,1);const uint8_t byte=0x42;
    for(unsigned i=0;i<4;++i)rin(f,14,RFCOMM_UIH,0,&byte,1,-1);
    assert(!f->client.rx_credits && !f->client.reserved_credits);
    const struct bt_rfcomm_buffer *r=bt_rfcomm_receive(&f->client);assert(r);
    assert(bt_rfcomm_release_receive(&f->client,r->serial)==1);
    f->d.lower.f.room=0;++f->d.lower.f.now;
    assert(bt_rfcomm_step(&f->client,64,UINT64_MAX)==1);wire(&f->d.lower);
    assert(!f->client.rx_credits && f->client.reserved_credits==1);
}
static void grant_publication_and_close_bound(void)
{
    struct serial_fixture f;drain_receive_credits(&f);
    uint8_t p[8],byte=0x55;unsigned n=reference_frame(p,14,RFCOMM_UIH,0,&byte,1,-1);
    /* The next poll can transmit the grant after RX. Its acquisition fence
     * still forbids this earlier uncredited frame. */
    rbytes(&f,p,n,0);assert(f.client.error==BT_RFCOMM_PROTOCOL && f.client.held_rx);
    drain_receive_credits(&f);expected_frame(&f,rout(&f),14,RFCOMM_UIH|0x10,1,0,0,1);
    assert(f.client.rx_credits==1 && !f.client.reserved_credits);
    rin(&f,14,RFCOMM_UIH,0,&byte,1,-1);assert(!f.client.rx_credits && f.client.phase==BT_RFCOMM_OPEN);
    assert(bt_rfcomm_close(&f.client,f.d.lower.f.now+2)==1);
    f.d.lower.f.room=0;++f.d.lower.f.now;assert(bt_rfcomm_step(&f.client,64,UINT64_MAX)==1);
    unsigned submitted=0;for(unsigned i=0;i<BT_RFCOMM_SLOTS;++i)submitted+=f.client.tx[i].state==BT_RFCOMM_SUBMITTED;
    assert(submitted);f.d.lower.f.now=f.client.close_deadline;f.d.lower.f.room=0;
    assert(bt_rfcomm_step(&f.client,64,UINT64_MAX)==-1 && f.client.error==BT_RFCOMM_EXPIRED);
    assert(f.registry.session[0]==&f.client && bt_rfcomm_release(&f.client)==-1);
}
static void parameter_refusal(void)
{
    struct serial_fixture f;rsetup(&f);rin(&f,0,RFCOMM_UA|0x10,1,0,0,-1);
    const uint8_t pn[]={14,0xf0,7,0,160,0,0,4};rexpect_mcc(&f,RFCOMM_PN,1,pn,8);
    uint8_t p[8];unsigned n=reference_frame(p,14,RFCOMM_DM|0x10,1,0,0,-1);rbytes(&f,p,n,0);
    assert(f.client.error==BT_RFCOMM_REFUSED && f.client.held_rx && f.registry.session[0]==&f.client);
}
int main(void)
{
    assert(sizeof(struct rfcomm_pn)==8 && sizeof(struct rfcomm_rpn)==8 && sizeof(struct rfcomm_msc)==2);
    data_credit_and_close();peer_close_and_collision();timeout_security_and_invalid();
    delayed_observation();grant_publication_and_close_bound();parameter_refusal();
    puts("RFCOMM source-byte framing/FCS, actual SDP/UART/HCI/ACL/L2CAP setup, independent credits, retained ownership, close, timers, security and malformed-frame checks passed.");return 0;
}
