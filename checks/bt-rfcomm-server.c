/* SPDX-License-Identifier: Apache-2.0 */
/* Reuse wire helpers without executing the preceding client test scenarios. */
#include "rfcomm-fixture.h"
static int server_authority(void *p,uint64_t link,uint16_t uuid,uint8_t server)
{struct serial_fixture *f=p;++f->calls;assert(link==1 && uuid==0x111f && server==7);return f->allowed ? 1 : -1;}
static void listener(struct serial_fixture *f)
{
    memset(f,0,sizeof(*f));f->allowed=1;struct simulation *s=&f->d.lower;
    ready(&s->f,&s->a);s->link=begin_connect(&s->f,&s->a,1);connected(&s->f,&s->a,s->link,0x23);
    s->wire_cursor=s->f.tn;s->automatic_credits=1;
    assert(bt_rfcomm_registry_init(&f->registry,&s->q,authorize,server_authority,f)==1);
    assert(bt_l2cap_init(&s->q,&s->a,672,1000000,60000000,bt_rfcomm_admit,&f->registry)==1);
    /* An unrelated PSM remains owned and open during server admission. */
    assert(open_channel(s)==1);f->d.next_output=s->outputs;
    f->allowed=0;assert(bt_rfcomm_listen(&f->client,&f->registry,1,0x111f,7,160,100000000)==-1);
    assert(!f->client.channel && !f->registry.session[0]);f->allowed=1;
    assert(bt_rfcomm_listen(&f->client,&f->registry,1,0x111f,7,160,100000000)==1);
    uint64_t direct=0;assert(bt_l2cap_open(&s->q,1,3,100000000,&direct)==-1 && !direct);
    assert(bt_rfcomm_admit(&f->registry,1,1,1)==1 && s->q.channels[0].state==BT_L2CAP_OPEN);
}
static void server_l2(struct serial_fixture *f,unsigned mtu)
{
    struct simulation *s=&f->d.lower;struct l2cap_conn_req request={3,0x61};
    signal_in(s,L2CAP_CONN_REQ,50,&request,4);unsigned out=rout(f);
    const struct l2cap_cmd_hdr *reply=command_out(s,out,L2CAP_CONN_RSP);
    struct l2cap_conn_rsp expected={0x41,0x61,0,0};assert(!memcmp(reply+1,&expected,8));
    assert(f->client.channel==2 && f->client.admission==2 && f->client.phase==BT_RFCOMM_OPENING);
    out=rout(f);const struct l2cap_cmd_hdr *config=command_out(s,out,L2CAP_CONF_REQ);
    const uint8_t ok[]={0x41,0,0,0,0,0};signal_in(s,L2CAP_CONF_RSP,config->ident,ok,6);rpoll(f);
    uint8_t peer[]={0x41,0,0,0,1,2,(uint8_t)mtu,(uint8_t)(mtu>>8)};signal_in(s,L2CAP_CONF_REQ,51,peer,8);
    command_out(s,rout(f),L2CAP_CONF_RSP);assert(f->client.phase==BT_RFCOMM_NEGOTIATING);
}
static void server_mcc(struct serial_fixture *f,unsigned type,unsigned command,const uint8_t *p,unsigned n)
{uint8_t bytes[BT_RFCOMM_MTU];unsigned length=reference_mcc(bytes,type,command,p,n);rin(f,0,RFCOMM_UIH,1,bytes,length,-1);}
static void server_expect(struct serial_fixture *f,unsigned type,unsigned command,const uint8_t *p,unsigned n)
{uint8_t bytes[BT_RFCOMM_MTU];unsigned length=reference_mcc(bytes,type,command,p,n);expected_frame(f,rout(f),0,RFCOMM_UIH,0,bytes,length,-1);}
static void server_open(struct serial_fixture *f,unsigned mtu)
{
    listener(f);server_l2(f,mtu);rin(f,0,RFCOMM_SABM|0x10,1,0,0,-1);
    expected_frame(f,rout(f),0,RFCOMM_UA|0x10,1,0,0,-1);assert(f->client.mux_open);
    const uint8_t pn[]={14,0xff,7,9,200,0,9,2};server_mcc(f,RFCOMM_PN,1,pn,8);
    unsigned selected=mtu<166 ? mtu-6 : 160;
    uint8_t response[]={14,0xe0,7,0,(uint8_t)selected,(uint8_t)(selected>>8),0,4};
    server_expect(f,RFCOMM_PN,0,response,8);assert(f->client.pn_ready && f->client.rx_credits==4);
    rin(f,14,RFCOMM_SABM|0x10,1,0,0,-1);expected_frame(f,rout(f),14,RFCOMM_UA|0x10,1,0,0,-1);
    assert(f->client.dlc_open);const uint8_t msc[]={59,0x8d};server_expect(f,RFCOMM_MSC,1,msc,2);
    server_mcc(f,RFCOMM_MSC,0,msc,2);assert(f->client.phase==BT_RFCOMM_NEGOTIATING);
    server_mcc(f,RFCOMM_MSC,1,msc,2);server_expect(f,RFCOMM_MSC,0,msc,2);rpoll(f);
    assert(f->client.phase==BT_RFCOMM_OPEN && f->client.mtu==selected && f->client.tx_credits==2);
}
static void server_data_and_close(void)
{
    struct serial_fixture f;server_open(&f,64);
    /* No second data DLC or L2CAP multiplexer can acquire this listener. */
    rin(&f,16,RFCOMM_SABM|0x10,1,0,0,-1);expected_frame(&f,rout(&f),16,RFCOMM_DM|0x10,1,0,0,-1);
    struct l2cap_conn_req collision={3,0x62};signal_in(&f.d.lower,L2CAP_CONN_REQ,52,&collision,4);
    const struct l2cap_cmd_hdr *r=command_out(&f.d.lower,rout(&f),L2CAP_CONN_RSP);
    assert(((const struct l2cap_conn_rsp *)(r+1))->result && f.client.phase==BT_RFCOMM_OPEN);
    const uint8_t command[]="AT+BRSF=0\r";rin(&f,14,RFCOMM_UIH,1,command,sizeof(command)-1,-1);
    const struct bt_rfcomm_buffer *received=bt_rfcomm_receive(&f.client);
    assert(received && received->length==sizeof(command)-1 && !memcmp(received->bytes,command,sizeof(command)-1));
    assert(bt_rfcomm_release_receive(&f.client,received->serial)==1);
    expected_frame(&f,rout(&f),14,RFCOMM_UIH|0x10,0,0,0,1);
    const uint8_t response[]="\r\nERROR\r\n";uint64_t serial;
    assert(bt_rfcomm_send(&f.client,response,sizeof(response)-1,100000000,&serial)==1);
    expected_frame(&f,rout(&f),14,RFCOMM_UIH,0,response,sizeof(response)-1,-1);
    assert(bt_rfcomm_result(&f.client)->serial==serial && bt_rfcomm_release_result(&f.client,serial)==1);
    f.d.lower.automatic_credits=0;rin(&f,0,RFCOMM_DISC|0x10,1,0,0,-1);
    expected_frame(&f,rout(&f),0,RFCOMM_UA|0x10,1,0,0,-1);
    assert(f.client.phase==BT_RFCOMM_CLOSING && !f.client.closing_lower && f.d.lower.a.credits==1);
    const uint8_t credit[]={1,0x23,0,1,0};event(&f.d.lower.f,HCI_EV_NUM_COMP_PKTS,credit,5);
    f.d.lower.automatic_credits=1;rpoll(&f);l2close(&f);
    assert(f.client.phase==BT_RFCOMM_CLOSED && bt_rfcomm_release(&f.client)==1);
    assert(f.d.lower.q.channels[0].state==BT_L2CAP_OPEN && bt_rfcomm_admit(&f.registry,1,1,1)==1);
}
static void before_and_after_admission(void)
{
    struct serial_fixture f;listener(&f);assert(bt_rfcomm_close(&f.client,100000000)==1);
    assert(f.client.phase==BT_RFCOMM_CLOSED && !f.client.channel && bt_rfcomm_release(&f.client)==1);
    assert(bt_rfcomm_admit(&f.registry,1,3,1)==0);
    listener(&f);struct l2cap_conn_req request={3,0x61};signal_in(&f.d.lower,L2CAP_CONN_REQ,50,&request,4);
    /* A different serialized worker advances the exact admitted channel first. */
    poll(&f.d.lower);assert(f.client.admission==2 && !f.client.channel && f.d.lower.q.channels[1].generation==2);
    assert(bt_rfcomm_close(&f.client,f.d.lower.f.now+2)==1);
    assert(f.client.channel==2 && f.client.phase==BT_RFCOMM_CLOSING);
    f.d.lower.f.now=f.client.close_deadline;assert(bt_rfcomm_step(&f.client,64,UINT64_MAX)==-1);
    assert(f.client.error==BT_RFCOMM_EXPIRED && f.registry.session[0]==&f.client && bt_rfcomm_release(&f.client)==-1);
    /* Expiring an unused listener releases no unrelated transport ownership. */
    listener(&f);f.d.lower.f.now=f.client.deadline;assert(bt_rfcomm_step(&f.client,64,UINT64_MAX)==0);
    assert(f.client.error==BT_RFCOMM_EXPIRED && f.client.phase==BT_RFCOMM_CLOSED && !f.client.channel);
    assert(bt_rfcomm_release(&f.client)==1 && f.d.lower.q.channels[0].state==BT_L2CAP_OPEN);
}
static void early_negotiation_and_security(void)
{
    struct serial_fixture f;listener(&f);server_l2(&f,672);
    uint8_t frame[8];unsigned n=reference_frame(frame,0,RFCOMM_SABM|0x10,1,0,0,-1);
    incoming_pdu(&f.d.lower,0x41,frame,n,0);f.d.lower.f.room=0;++f.d.lower.f.now;
    assert(bt_rfcomm_step(&f.client,64,UINT64_MAX)==1 && f.client.mux_pending && !f.client.mux_open);
    const uint8_t pn[]={14,0xf0,7,0,160,0,0,2};uint8_t command[16],bytes[24];
    unsigned cn=reference_mcc(command,RFCOMM_PN,1,pn,8);n=reference_frame(bytes,0,RFCOMM_UIH,1,command,cn,-1);
    rbytes(&f,bytes,n,0);assert(f.client.error==BT_RFCOMM_PROTOCOL && f.client.held_rx && !f.client.pn_pending);
    listener(&f);f.allowed=0;struct l2cap_conn_req request={3,0x61};signal_in(&f.d.lower,L2CAP_CONN_REQ,50,&request,4);
    /* Pumping L2CAP directly still invokes the listener's security authority. */
    unsigned old=f.d.lower.outputs,out=output(&f.d.lower);
    const struct l2cap_cmd_hdr *r=command_out(&f.d.lower,out,L2CAP_CONN_RSP);
    assert(f.d.lower.outputs==old+1 && ((const struct l2cap_conn_rsp *)(r+1))->result==3);
    assert(!f.client.admission && !f.client.channel && f.client.phase==BT_RFCOMM_LISTENING);
}
static void allocation_refusal_and_live_identity(void)
{
    struct serial_fixture f;listener(&f);struct simulation *s=&f.d.lower;
    uint64_t unrelated[3];for(unsigned i=0;i<3;++i)assert(bt_l2cap_open(&s->q,1,1,100000000,&unrelated[i])==1);
    struct l2cap_conn_req req={3,0x61};signal_in(s,L2CAP_CONN_REQ,50,&req,4);
    for(unsigned i=0;i<30;++i)poll(s);
    assert(f.client.admission==5 && s->q.next_channel==5 && !f.client.channel);
    rpoll(&f);assert(f.client.phase==BT_RFCOMM_LISTENING && !f.client.channel);
    struct l2cap_disconn_req pair={0x40,0x60};signal_in(s,L2CAP_DISCONN_REQ,51,&pair,4);
    for(unsigned i=0;i<20;++i)poll(s);
    assert(s->q.channels[0].state==BT_L2CAP_CLOSED && bt_l2cap_release_channel(&s->q,1)==1);
    uint64_t later;assert(bt_l2cap_open(&s->q,1,1,100000000,&later)==1 && later==5);
    rpoll(&f);assert(f.client.phase==BT_RFCOMM_LISTENING && !f.client.channel);
    assert(bt_rfcomm_close(&f.client,100000000)==1 && f.client.phase==BT_RFCOMM_CLOSED);
    assert(s->q.channels[0].generation==5 && s->q.channels[0].state==BT_L2CAP_CONNECTING);
    listener(&f);const uint8_t flush[]={0x23,0};event(&f.d.lower.f,0x11,flush,2);poll(&f.d.lower);
    assert(f.d.lower.a.links[0].flush_seen && bt_rfcomm_admit(&f.registry,1,3,1)==-1 && !f.client.admission);
    assert(bt_rfcomm_close(&f.client,100000000)==1 && bt_rfcomm_release(&f.client)==1);
    assert(bt_rfcomm_listen(&f.client,&f.registry,1,0x111f,7,160,100000000)==-1 && !f.registry.session[0]);
}
int main(void)
{
    server_data_and_close();before_and_after_admission();early_negotiation_and_security();
    allocation_refusal_and_live_identity();
    puts("Incoming RFCOMM responder direction, PN/MSC/credits, exact L2CAP admission, collision/security and retained close checks passed over native lower stack.");return 0;
}
