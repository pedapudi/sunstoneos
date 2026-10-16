#include "bt_l2cap.h"
#include "bt-ref.h"
#include "l2cap-ref.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct fixture {
    struct a733_uart uart;
    struct bt_hci hci;
    struct bt_h4_storage slots[3];
    uint8_t storage[3][1024], rx[8192], tx[32768];
    size_t rn, ri, tn;
    uint32_t regs[64];
    uint64_t now;
    unsigned room, accesses, error, uncertain;
    int current, immediate_reset_reply;
};
static int read32(void *p, uintptr_t address, uint32_t *v)
{
    struct fixture *f=p;unsigned o=(unsigned)(address-0x02501000u);++f->accesses;
    assert(!(o&3) && o<=0xa4);
    if(o==0) {assert(!(f->regs[3]&128) && f->ri<f->rn);*v=f->rx[f->ri++];}
    else if(o==0x14) {*v=0x60u|(f->ri<f->rn ? 1u : 0u)|f->error;f->error=0;}
    else if(o==0x7c) *v=4u|(f->room ? 2u : 0u)|(f->ri<f->rn ? 9u : 0u);
    else if(o==0xa4) *v=3;
    else *v=f->regs[o/4];
    return 1;
}
static int write32(void *p, uintptr_t address, uint32_t v)
{
    struct fixture *f=p;unsigned o=(unsigned)(address-0x02501000u);++f->accesses;
    assert(!(o&3) && (o<=0x10 || o==0xa4));
    if(o==0 && !(f->regs[3]&128)) {
        assert(f->room && f->tn<sizeof(f->tx));--f->room;f->tx[f->tn++]=(uint8_t)v;
        if(f->uncertain) return -1;
        if(f->immediate_reset_reply && f->tn==4) {
            const uint8_t response[]={4,14,4,1,3,12,0};
            assert(f->ri==f->rn);f->ri=0;f->rn=sizeof(response);
            memcpy(f->rx,response,sizeof(response));
        }
    } else if((o!=0 && o!=4) || !(f->regs[3]&128)) f->regs[o/4]=v;
    return 1;
}
static int now(void *p,uint64_t *v) {*v=((struct fixture *)p)->now;return 1;}
static int current(void *p,uint64_t firmware,uint64_t uart)
{ return ((struct fixture *)p)->current && firmware==4 && uart==9; }
static void init(struct fixture *f)
{
    memset(f,0,sizeof(*f));f->now=1;f->room=1024;f->current=1;
    struct a733_uart_format fmt={1,8,2,0,1,24000000,1500000,0};
    const struct a733_uart_ops ops={read32,write32,now};
    assert(a733_uart_begin(&f->uart,&ops,f,&fmt,10000)==A733_UART_CONFIGURING);
    assert(a733_uart_poll(&f->uart)==A733_UART_READY);
    for(unsigned i=0;i<3;++i)f->slots[i]=(struct bt_h4_storage){f->storage[i],sizeof(f->storage[i])};
    struct bt_h4_binding b={&f->uart,4,9,100,1500000,current,f};
    assert(bt_hci_init(&f->hci,&b,f->slots,3,10000,500)==1);
}
static void bytes(struct fixture *f,const void *p,size_t n)
{
    if(f->ri==f->rn)f->ri=f->rn=0;
    assert(f->rn+n<=sizeof(f->rx));memcpy(f->rx+f->rn,p,n);f->rn+=n;
}
static void event(struct fixture *f,unsigned code,const void *p,unsigned n)
{
    uint8_t type=HCI_EVENT_PKT;
    struct hci_event_hdr header={(uint8_t)code,(uint8_t)n};
    bytes(f,&type,1);bytes(f,&header,sizeof(header));if(n)bytes(f,p,n);
}
static void cc(struct fixture *f,unsigned opcode,unsigned credits,unsigned retlen,unsigned status)
{
    uint8_t params[70]={0};
    struct hci_ev_cmd_complete c={(uint8_t)credits,(uint16_t)opcode};
    memcpy(params,&c,sizeof(c));
    if(retlen) {
        params[sizeof(c)]=(uint8_t)status;
        for(unsigned i=1;i<retlen;++i)params[sizeof(c)+i]=(uint8_t)(0x30+i);
    }
    if(opcode==HCI_OP_READ_LOCAL_COMMANDS && retlen==65) params[4]=0x30;
    if(opcode==HCI_OP_READ_LOCAL_FEATURES && retlen==9) params[8]=0;
    if(opcode==HCI_OP_READ_BUFFER_SIZE && retlen==8) {
        const uint8_t capacity[]={32,0,0,2,0,0,0};memcpy(params+4,capacity,7);
    }
    event(f,HCI_EV_CMD_COMPLETE,params,sizeof(c)+retlen);
}
static void cs(struct fixture *f,unsigned opcode,unsigned credits,unsigned status)
{
    struct hci_ev_cmd_status s={(uint8_t)status,(uint8_t)credits,(uint16_t)opcode};
    event(f,HCI_EV_CMD_STATUS,&s,sizeof(s));
}
static void step(struct fixture *f,unsigned budget)
{assert(bt_hci_step(&f->hci,budget,10000)==1);}
static void send(struct fixture *f,unsigned opcode)
{
    if(f->hci.command==BT_HCI_IDLE)step(f,64);
    size_t before=f->tn;
    assert(f->hci.command==BT_HCI_SENDING);
    step(f,64);
    assert(f->hci.command==BT_HCI_WAITING && f->tn==before+4);
    uint8_t ref[4]={HCI_COMMAND_PKT,(uint8_t)opcode,(uint8_t)(opcode>>8),0};
    assert(!memcmp(ref,f->tx+before,4));
}
static void initialize_all(struct fixture *f)
{
    const unsigned op[]={HCI_OP_RESET,HCI_OP_READ_LOCAL_VERSION,HCI_OP_READ_LOCAL_COMMANDS,
        HCI_OP_READ_LOCAL_FEATURES,HCI_OP_READ_BUFFER_SIZE,HCI_OP_READ_BD_ADDR};
    const unsigned sizes[]={1,sizeof(struct hci_rp_read_local_version),
        sizeof(struct hci_rp_read_local_commands),sizeof(struct hci_rp_read_local_features),
        sizeof(struct hci_rp_read_buffer_size),sizeof(struct hci_rp_read_bd_addr)};
    for(unsigned i=0;i<6;++i) {
        send(f,op[i]);cc(f,op[i],1,sizes[i],0);
        for(unsigned j=0;j<4 && f->hci.stage==i;++j)step(f,64);
        assert(f->hci.stage==i+1);
    }
    assert(f->hci.state==BT_HCI_IDENTIFIED && f->hci.identity.valid==63);
    assert(f->hci.identity.supported_commands[63]==0x70 && f->hci.identity.address[5]==0x36);
}
static void astep(struct bt_acl *a) {assert(bt_acl_step(a,64,10000)==1);}
static void ready(struct fixture *f,struct bt_acl *a)
{ init(f);initialize_all(f);memset(a,0,sizeof(*a));assert(bt_acl_init(a,&f->hci)==1); }
static uint64_t begin_connect(struct fixture *f,struct bt_acl *a,unsigned address)
{
    struct bt_acl_peer peer={{(uint8_t)address,2,3,4,5,6},1,0,0x8123};uint64_t generation=0;
    assert(bt_acl_connect(a,&peer,5000,&generation)==1);astep(a);
    size_t before=f->tn;astep(a);assert(f->hci.command==BT_HCI_WAITING);
    struct hci_cp_create_conn ref={{.b={(uint8_t)address,2,3,4,5,6}},0x3306,1,0,0x8123,0};
    const uint8_t header[]={HCI_COMMAND_PKT,5,4,sizeof(ref)};
    assert(f->tn==before+sizeof(header)+sizeof(ref));
    assert(!memcmp(f->tx+before,header,sizeof(header)) && !memcmp(f->tx+before+4,&ref,sizeof(ref)));
    assert(a->links[generation-1].state==BT_ACL_CONNECT_STATUS);
    return generation;
}
static void connected(struct fixture *f,struct bt_acl *a,uint64_t generation,unsigned h)
{
    cs(f,HCI_OP_CREATE_CONN,1,0);astep(a);
    assert(a->links[generation-1].state==BT_ACL_CONNECTING);
    struct hci_ev_conn_complete e={0,(uint16_t)h,{{0}},1,0};
    memcpy(e.bdaddr.b,a->links[generation-1].peer.address,6);
    event(f,HCI_EV_CONN_COMPLETE,&e,sizeof(e));astep(a);
    assert(a->links[generation-1].state==BT_ACL_CONNECTED);
}
struct simulation {
    struct fixture f;
    struct bt_acl a;
    struct bt_l2cap q;
    uint64_t link;
    size_t wire_cursor, assembling;
    uint8_t assembly[BT_L2CAP_MTU+4], output[48][BT_L2CAP_MTU+4];
    size_t output_length[48];unsigned outputs, complete_frames;
    int automatic_credits;
};
static int authorize(void *context,uint64_t link,uint16_t psm,unsigned incoming)
{ (void)context;(void)incoming;assert(link==1);return psm==1 || psm==3 ? 1 : psm==5 ? -1 : 0; }
static void setup(struct simulation *s)
{
    memset(s,0,sizeof(*s));ready(&s->f,&s->a);
    s->link=begin_connect(&s->f,&s->a,1);connected(&s->f,&s->a,s->link,0x23);
    s->wire_cursor=s->f.tn;s->automatic_credits=1;
    assert(bt_l2cap_init(&s->q,&s->a,672,1000000,authorize,s)==1);
}
static void wire(struct simulation *s)
{
    while(s->f.tn-s->wire_cursor>=5) {
        const uint8_t *p=s->f.tx+s->wire_cursor;assert(p[0]==HCI_ACLDATA_PKT);
        struct hci_acl_hdr header;memcpy(&header,p+1,4);
        unsigned n=header.dlen;assert((header.handle&0xfff)==0x23);
        if(s->f.tn-s->wire_cursor<5+n)break;
        if((header.handle>>12)==2) {assert(!s->assembling);}
        else assert((header.handle>>12)==1 && s->assembling);
        assert(s->assembling+n<=sizeof(s->assembly));memcpy(s->assembly+s->assembling,p+5,n);s->assembling+=n;
        if(s->assembling>=4) {
            struct l2cap_hdr hdr;memcpy(&hdr,s->assembly,4);assert(s->assembling<=(size_t)hdr.len+4);
            if(s->assembling==(size_t)hdr.len+4) {
                assert(s->outputs<48);memcpy(s->output[s->outputs],s->assembly,s->assembling);
                s->output_length[s->outputs++]=s->assembling;s->assembling=0;
            }
        }
        ++s->complete_frames;s->wire_cursor+=5+n;
        if(s->automatic_credits) {
            const uint8_t credit[]={1,0x23,0,1,0};event(&s->f,HCI_EV_NUM_COMP_PKTS,credit,sizeof(credit));
        }
    }
}
static void poll(struct simulation *s)
{
    s->f.room=1024;++s->f.now;
    int result=bt_l2cap_step(&s->q,64,10000);
    if(result!=1)fprintf(stderr,"L2CAP failure state=%u error=%u now=%llu\n",s->q.state,s->q.error,(unsigned long long)s->f.now);
    assert(result==1);wire(s);
}
static unsigned output(struct simulation *s)
{
    unsigned target=s->outputs;
    for(unsigned i=0;i<100 && s->outputs==target;++i)poll(s);
    assert(s->outputs==target+1);
    for(unsigned i=0;i<5;++i)poll(s);
    return target;
}
static void incoming_fragment(struct simulation *s,unsigned pb,const void *payload,size_t n)
{
    const uint8_t type=HCI_ACLDATA_PKT;struct hci_acl_hdr hdr={(uint16_t)(0x23|(pb<<12)),(uint16_t)n};
    bytes(&s->f,&type,1);bytes(&s->f,&hdr,4);bytes(&s->f,payload,n);
}
static void incoming_pdu(struct simulation *s,unsigned cid,const void *payload,size_t n,unsigned split)
{
    uint8_t all[BT_L2CAP_MTU+4];struct l2cap_hdr hdr={(uint16_t)n,(uint16_t)cid};
    memcpy(all,&hdr,4);if(n)memcpy(all+4,payload,n);
    if(split) {
        incoming_fragment(s,2,all,1);poll(s);incoming_fragment(s,1,all+1,3);poll(s);
        incoming_fragment(s,1,all+4,n);
    } else incoming_fragment(s,2,all,n+4);
}
static void signal_in(struct simulation *s,unsigned code,unsigned id,const void *p,size_t n)
{
    uint8_t cmd[BT_L2CAP_SIGNAL_MTU];struct l2cap_cmd_hdr hdr={(uint8_t)code,(uint8_t)id,(uint16_t)n};
    memcpy(cmd,&hdr,4);if(n)memcpy(cmd+4,p,n);incoming_pdu(s,1,cmd,n+4,0);
}
static const struct l2cap_cmd_hdr *command_out(struct simulation *s,unsigned out,unsigned code)
{
    struct l2cap_hdr h;memcpy(&h,s->output[out],4);assert(h.cid==L2CAP_CID_SIGNALING && h.len+4==s->output_length[out]);
    const struct l2cap_cmd_hdr *cmd=(const void *)(s->output[out]+4);assert(cmd->code==code && cmd->len+4==h.len);
    return cmd;
}
static uint16_t open_channel(struct simulation *s)
{
    uint16_t cid=0;assert(bt_l2cap_open(&s->q,s->link,1,5000,&cid)==1);
    unsigned out=output(s);const struct l2cap_cmd_hdr *req=command_out(s,out,L2CAP_CONN_REQ);
    struct l2cap_conn_req expected={L2CAP_PSM_SDP,cid};assert(req->len==sizeof(expected));
    assert(!memcmp((const uint8_t *)req+4,&expected,sizeof(expected)));
    struct l2cap_conn_rsp rsp={0x60,cid,0,0};signal_in(s,L2CAP_CONN_RSP,req->ident,&rsp,sizeof(rsp));
    out=output(s);req=command_out(s,out,L2CAP_CONF_REQ);unsigned config_id=req->ident;
    const uint8_t config[]={0x60,0,0,0,1,2,0xa0,2};assert(req->len==8 && !memcmp((const uint8_t *)req+4,config,8));
    const uint8_t accept[]={0x40,0,0,0,0,0};signal_in(s,L2CAP_CONF_RSP,config_id,accept,sizeof(accept));poll(s);
    assert(s->q.channels[0].state==BT_L2CAP_CONFIGURING && s->q.channels[0].input_done);
    const uint8_t peer[]={0x40,0,0,0,1,2,80,0};signal_in(s,L2CAP_CONF_REQ,40,peer,sizeof(peer));
    poll(s);assert(s->q.channels[0].state==BT_L2CAP_CONFIGURING);
    out=output(s);req=command_out(s,out,L2CAP_CONF_RSP);
    const uint8_t positive[]={0x60,0,0,0,0,0};assert(req->len==6 && !memcmp((const uint8_t *)req+4,positive,6));
    assert(s->q.channels[0].state==BT_L2CAP_OPEN && s->q.channels[0].remote_mtu==80);
    return cid;
}
static void data_and_close(void)
{
    struct simulation s;setup(&s);uint16_t cid=open_channel(&s);
    uint8_t payload[80];for(unsigned i=0;i<sizeof(payload);++i)payload[i]=(uint8_t)(i+1);
    uint64_t serial;assert(bt_l2cap_send(&s.q,cid,payload,sizeof(payload),5000,&serial)==1);
    unsigned frame_before=s.complete_frames,out=output(&s);struct l2cap_hdr hdr={80,0x60};
    assert(s.output_length[out]==84 && !memcmp(s.output[out],&hdr,4) && !memcmp(s.output[out]+4,payload,80));
    assert(s.complete_frames-frame_before==3 && bt_l2cap_result(&s.q)->serial==serial);
    assert(bt_l2cap_release_result(&s.q,serial)==1 && s.a.credits==2);
    /* An unrelated security event stays owned while fragmented ACL progresses. */
    const uint8_t auth[]={0,0x23,0};event(&s.f,6,auth,sizeof(auth));poll(&s);
    incoming_pdu(&s,cid,payload,50,1);poll(&s);
    const struct bt_l2cap_pdu *received=bt_l2cap_receive(&s.q);
    assert(received && received->cid==cid && received->link==s.link && received->length==54 && !memcmp(received->bytes+4,payload,50));
    assert(bt_l2cap_other(&s.q) && bt_l2cap_release_other(&s.q,bt_l2cap_other(&s.q)->serial)==1);
    assert(bt_l2cap_release_receive(&s.q,received->serial)==1);
    assert(bt_l2cap_close(&s.q,cid,UINT64_MAX)==1);
    assert(s.q.channels[0].deadline==s.f.now+s.q.timeout_us);out=output(&s);
    const struct l2cap_cmd_hdr *disc=command_out(&s,out,L2CAP_DISCONN_REQ);
    struct l2cap_disconn_req pair={0x60,cid};assert(!memcmp((const uint8_t *)disc+4,&pair,4));
    signal_in(&s,L2CAP_DISCONN_RSP,disc->ident,&pair,4);poll(&s);
    assert(s.q.channels[0].state==BT_L2CAP_CLOSED && bt_l2cap_send(&s.q,cid,payload,1,6000,&serial)==-1);
}
static void incoming_and_duplicate(void)
{
    struct simulation s;setup(&s);
    struct l2cap_conn_req req={L2CAP_PSM_RFCOMM,0x61};signal_in(&s,L2CAP_CONN_REQ,20,&req,4);
    unsigned out=output(&s);const struct l2cap_cmd_hdr *r=command_out(&s,out,L2CAP_CONN_RSP);
    struct l2cap_conn_rsp expected={0x40,0x61,0,0};assert(!memcmp((const uint8_t *)r+4,&expected,8));
    /* Local configuration can already follow the reply in the wire log. */
    if(s.outputs<2)output(&s);const struct l2cap_cmd_hdr *conf=command_out(&s,1,L2CAP_CONF_REQ);
    const uint8_t accept[]={0x40,0,0,0,0,0};signal_in(&s,L2CAP_CONF_RSP,conf->ident,accept,6);poll(&s);
    unsigned before=s.outputs;signal_in(&s,L2CAP_CONN_REQ,20,&req,4);out=output(&s);
    assert(out==before && s.output_length[out]==s.output_length[0] && !memcmp(s.output[out],s.output[0],s.output_length[0]));
    assert(s.q.channels[1].state==BT_L2CAP_UNUSED);
    /* Complete configuration TLVs span requests with distinct IDs. */
    const uint8_t part[]={0x40,0,1,0,1,2,100,0};signal_in(&s,L2CAP_CONF_REQ,21,part,sizeof(part));
    out=output(&s);r=command_out(&s,out,L2CAP_CONF_RSP);assert(((const uint8_t *)r)[6]==1 && s.q.channels[0].state==BT_L2CAP_CONFIGURING);
    const uint8_t end[]={0x40,0,0,0};signal_in(&s,L2CAP_CONF_REQ,22,end,sizeof(end));output(&s);
    assert(s.q.channels[0].state==BT_L2CAP_OPEN && s.q.channels[0].remote_mtu==100);
    struct l2cap_disconn_req pair={0x40,0x61};signal_in(&s,L2CAP_DISCONN_REQ,23,&pair,4);
    out=output(&s);r=command_out(&s,out,L2CAP_DISCONN_RSP);assert(!memcmp((const uint8_t *)r+4,&pair,4));
    assert(s.q.channels[0].state==BT_L2CAP_CLOSED);
    signal_in(&s,L2CAP_DISCONN_REQ,23,&pair,4);output(&s);
    /* A fresh identifier cannot rebind a quarantined peer endpoint. */
    signal_in(&s,L2CAP_CONN_REQ,24,&req,4);out=output(&s);r=command_out(&s,out,L2CAP_CONN_RSP);
    assert(((const uint8_t *)r)[8]==7 && s.q.channels[1].state==BT_L2CAP_UNUSED);
}
static void early_response_and_fault(void)
{
    struct simulation s;setup(&s);uint16_t cid;
    assert(bt_l2cap_open(&s.q,s.link,1,5000,&cid)==1);
    struct l2cap_conn_rsp rsp={0x60,cid,0,0};signal_in(&s,L2CAP_CONN_RSP,1,&rsp,8);poll(&s);
    assert(s.q.channels[0].state==BT_L2CAP_CONNECTING);output(&s);
    signal_in(&s,L2CAP_CONN_RSP,1,&rsp,8);output(&s);
    assert(s.q.channels[0].state==BT_L2CAP_CONFIGURING);
    /* A continuation for another handle never appends to this link's buffer. */
    uint8_t partial=0;incoming_fragment(&s,2,&partial,1);poll(&s);
    const uint8_t invalid[]={2,0x24,0x10,1,0,0};bytes(&s.f,invalid,sizeof(invalid));
    assert(bt_l2cap_step(&s.q,64,10000)==-1 && s.q.error==BT_L2CAP_OWNERSHIP);
    assert(s.q.rx[s.q.assembling[0]].length==1 && bt_l2cap_release_receive(&s.q,1)==-1);
}
static void unknown_options_and_bounds(void)
{
    struct simulation s;setup(&s);uint16_t cid=open_channel(&s);
    /* Unknown mandatory TLV returns an exact copy; hints need no rejection. */
    const uint8_t unknown[]={0x40,0,0,0,0x20,2,0xab,0xcd};signal_in(&s,L2CAP_CONF_REQ,41,unknown,sizeof(unknown));
    unsigned out=output(&s);const struct l2cap_cmd_hdr *r=command_out(&s,out,L2CAP_CONF_RSP);
    const uint8_t expected[]={0x60,0,0,0,3,0,0x20,2,0xab,0xcd};
    assert(r->len==sizeof(expected) && !memcmp((const uint8_t *)r+4,expected,sizeof(expected)));
    uint64_t serial=99;uint8_t too_long[81]={0};assert(bt_l2cap_send(&s.q,cid,too_long,81,5000,&serial)==-1 && serial==99);
    /* Invalid second signaling command causes no first-command mutation. */
    const uint8_t malformed[]={8,42,0,0,2,43,4,0,1};incoming_pdu(&s,1,malformed,sizeof(malformed),0);
    unsigned requests=0;for(unsigned i=0;i<BT_L2CAP_REQUESTS;++i)requests+=!!s.q.requests[i].length;
    assert(bt_l2cap_step(&s.q,64,10000)==-1 && s.q.error==BT_L2CAP_PROTOCOL);
    unsigned after=0;for(unsigned i=0;i<BT_L2CAP_REQUESTS;++i)after+=!!s.q.requests[i].length;
    assert(after==requests);
}
static void bounded_lifetime(void)
{
    struct simulation s;setup(&s);uint16_t cid;
    assert(bt_l2cap_open(&s.q,s.link,1,UINT64_MAX,&cid)==1);
    assert(s.q.channels[0].deadline==s.f.now+s.q.timeout_us);
    output(&s);s.f.now=s.q.channels[0].deadline;
    assert(bt_l2cap_step(&s.q,64,UINT64_MAX)==-1 && s.q.state==BT_L2CAP_FAULT &&
           s.q.channels[0].state==BT_L2CAP_CONNECTING);
    setup(&s);
    for(unsigned i=0;i<BT_L2CAP_REQUESTS;++i) {
        signal_in(&s,L2CAP_ECHO_REQ,i+1,0,0);output(&s);
    }
    signal_in(&s,L2CAP_ECHO_REQ,17,0,0);
    assert(bt_l2cap_step(&s.q,64,10000)==BT_L2CAP_EXHAUSTED &&
           s.q.error==BT_L2CAP_CAPACITY && s.q.state==BT_L2CAP_FAULT);
}
int main(void)
{
    data_and_close();incoming_and_duplicate();early_response_and_fault();unknown_options_and_bounds();bounded_lifetime();
    puts("Native UART/HCI/ACL/L2CAP: channel negotiation both directions, source-defined bytes, fragments/credits, duplicate/late signaling and retained faults passed.");
    return 0;
}
