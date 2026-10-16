#include "bt_acl.h"
#include "bt-ref.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct fixture {
    struct a733_uart uart;
    struct bt_hci hci;
    struct bt_h4_storage slots[3];
    uint8_t storage[3][1024], rx[4096], tx[4096];
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
    if(opcode==HCI_OP_READ_LOCAL_COMMANDS && retlen==65) {params[4]=0x30;params[5]=3;}
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
static uint64_t queue(struct bt_acl *a,uint64_t generation,const uint8_t *p,size_t n)
{
    uint64_t serial=0;assert(bt_acl_submit(a,generation,2,p,n,5000,&serial)==1);return serial;
}
static void sent(struct fixture *f,struct bt_acl *a,const uint8_t *payload,size_t n,unsigned h)
{
    astep(a);assert(a->active_tx<BT_ACL_TX_SLOTS);
    size_t before=f->tn;astep(a);assert(f->tn==before+5);
    assert(a->packets[a->active_tx].state==BT_ACL_PARTIAL);astep(a);
    assert(a->active_tx==BT_ACL_TX_SLOTS && f->tn==before+n+5);
    struct hci_acl_hdr header={(uint16_t)(h|0x2000),(uint16_t)n};
    assert(f->tx[before]==HCI_ACLDATA_PKT && !memcmp(f->tx+before+1,&header,4));
    assert(!memcmp(f->tx+before+5,payload,n));
}
static void completed(struct fixture *f,const struct hci_comp_pkts_info *pairs,unsigned n)
{
    uint8_t p[1+4*BT_ACL_LINKS];p[0]=(uint8_t)n;memcpy(p+1,pairs,n*sizeof(*pairs));
    event(f,HCI_EV_NUM_COMP_PKTS,p,1+n*sizeof(*pairs));
}
static void disconnect_event(struct fixture *f,unsigned h)
{
    struct hci_ev_disconn_complete d={0,(uint16_t)h,0x13};
    event(f,HCI_EV_DISCONN_COMPLETE,&d,sizeof(d));
}
static void enable(struct fixture *f,struct bt_acl *a)
{ ready(f,a);assert(bt_acl_incoming(a,2000)==1); }
static uint64_t listen_peer(struct bt_acl *a,unsigned address)
{
    uint8_t peer[]={address,2,3,4,5,6};uint64_t generation=0;
    assert(bt_acl_listen(a,peer,5000,&generation)==1);
    peer[0]=99; /* Caller storage is not retained. */
    assert(a->links[generation-1].peer.address[0]==address);
    return generation;
}
static void request(struct fixture *f,const uint8_t address[6],unsigned type)
{
    struct hci_ev_conn_request r={{{0}},{0x04,0x04,0x24},(uint8_t)type};
    memcpy(r.bdaddr.b,address,6);event(f,HCI_EV_CONN_REQUEST,&r,sizeof(r));
}
static void command(struct fixture *f,struct bt_acl *a,unsigned slot,unsigned opcode)
{
    assert(f->hci.command==BT_HCI_SENDING);size_t before=f->tn;
    astep(a);assert(f->hci.command==BT_HCI_WAITING && f->tn==before+11);
    uint8_t header[]={HCI_COMMAND_PKT,opcode,opcode>>8,7};
    assert(!memcmp(header,f->tx+before,4));
    if(opcode==HCI_OP_ACCEPT_CONN_REQ) {
        struct hci_cp_accept_conn_req p={{{0}},1};
        memcpy(p.bdaddr.b,a->links[slot].peer.address,6);
        assert(!memcmp(f->tx+before+4,&p,sizeof(p)));
    } else {
        struct hci_cp_reject_conn_req p={{{0}},a->links[slot].reason};
        memcpy(p.bdaddr.b,a->links[slot].peer.address,6);
        assert(!memcmp(f->tx+before+4,&p,sizeof(p)));
    }
}
static void terminal(struct fixture *f,const struct bt_acl_link *l,unsigned status,unsigned h)
{
    struct hci_ev_conn_complete e={(uint8_t)status,(uint16_t)h,{{0}},1,0};
    memcpy(e.bdaddr.b,l->peer.address,6);event(f,HCI_EV_CONN_COMPLETE,&e,sizeof(e));
}
static void accept_link(struct fixture *f,struct bt_acl *a,unsigned slot,unsigned h)
{
    request(f,a->links[slot].peer.address,1);astep(a);
    command(f,a,slot,HCI_OP_ACCEPT_CONN_REQ);
    cs(f,HCI_OP_ACCEPT_CONN_REQ,1,0);astep(a);
    assert(a->links[slot].state==BT_ACL_ACCEPTING);
    terminal(f,&a->links[slot],0,h);astep(a);
    assert(a->links[slot].state==BT_ACL_CONNECTED);
}
static void incoming_data(void)
{
    struct fixture f;struct bt_acl a;enable(&f,&a);uint64_t g=listen_peer(&a,1);
    accept_link(&f,&a,0,0x23);
    assert(a.links[0].generation==g && a.links[0].incoming && a.links[0].request_serial &&
        a.links[0].deadline==2001 && !a.links[0].encryption && a.links[0].class_of_device[2]==0x24);
    /* Actual pairing request remains owned, without changing link security. */
    event(&f,0x17,a.links[0].peer.address,6);astep(&a);
    const struct bt_h4_record *r=bt_acl_receive(&a);assert(r && r->storage.bytes[0]==0x17);
    uint64_t rs=r->serial;const uint8_t data[]={1,2,3};uint64_t tx=queue(&a,g,data,3);
    sent(&f,&a,data,3,0x23);struct hci_comp_pkts_info c={0x23,1};completed(&f,&c,1);astep(&a);
    assert(a.credits==2 && bt_acl_result(&a)->serial==tx && bt_acl_release(&a,tx)==1);
    assert(bt_acl_receive(&a)->serial==rs && !a.links[0].encryption);
    assert(bt_acl_receive_release(&a,rs)==1);
    disconnect_event(&f,0x23);astep(&a);assert(a.links[0].state==BT_ACL_CLOSED);
    uint64_t untouched=99;uint8_t peer[]={1,2,3,4,5,6};
    assert(bt_acl_listen(&a,peer,5000,&untouched)==-1 && untouched==99);
}
static void rejected(void)
{
    struct fixture f;struct bt_acl a;enable(&f,&a);uint8_t peer[]={7,2,3,4,5,6};
    request(&f,peer,1);astep(&a);assert(a.links[0].reason==0x0e);
    command(&f,&a,0,HCI_OP_REJECT_CONN_REQ);cs(&f,HCI_OP_REJECT_CONN_REQ,1,0);astep(&a);
    assert(a.links[0].state==BT_ACL_REJECTING && a.links[0].handle==0xffff);
    terminal(&f,&a.links[0],0x16,0);astep(&a);assert(a.links[0].state==BT_ACL_CLOSED);
    request(&f,f.hci.identity.address,1);astep(&a);assert(a.links[1].reason==0x0f);
    command(&f,&a,1,HCI_OP_REJECT_CONN_REQ);cs(&f,HCI_OP_REJECT_CONN_REQ,1,0x0c);astep(&a);
    assert(a.links[1].state==BT_ACL_CLOSED && a.links[1].status==0x0c && a.credits==2);
    request(&f,peer,1);assert(bt_acl_step(&a,64,10000)==-1 && a.links[0].state==BT_ACL_CLOSED);
}
static void interleaved(void)
{
    struct fixture f;struct bt_acl a;enable(&f,&a);
    uint64_t outgoing=begin_connect(&f,&a,1),incoming=listen_peer(&a,7);
    request(&f,a.links[1].peer.address,1);astep(&a);
    assert(a.links[1].state==BT_ACL_ACCEPT_QUEUED && a.command_link==0);
    /* Status for the wrong opcode cannot complete the outstanding Create. */
    cs(&f,HCI_OP_ACCEPT_CONN_REQ,4,0);astep(&a);
    assert(a.command_link==0 && !f.hci.credits);
    cs(&f,HCI_OP_CREATE_CONN,1,0);astep(&a);
    assert(a.links[0].state==BT_ACL_CONNECTING && a.command_link==1);
    command(&f,&a,1,HCI_OP_ACCEPT_CONN_REQ);
    /* The outbound terminal is independent of the incoming command. */
    terminal(&f,&a.links[0],0,0x23);astep(&a);
    assert(a.links[outgoing-1].state==BT_ACL_CONNECTED && a.command_link==1);
    cs(&f,HCI_OP_ACCEPT_CONN_REQ,1,0);astep(&a);
    terminal(&f,&a.links[1],0,0x24);astep(&a);
    assert(a.links[incoming-1].state==BT_ACL_CONNECTED && a.credits==2);
    const struct bt_h4_record *r=bt_acl_receive(&a);
    assert(r && r->storage.bytes[0]==HCI_EV_CMD_STATUS && bt_acl_receive_release(&a,r->serial)==1);
    /* Multiple queued requests cannot displace each other's command owner. */
    listen_peer(&a,8);listen_peer(&a,9);
    request(&f,a.links[2].peer.address,1);request(&f,a.links[3].peer.address,1);
    astep(&a);assert(a.command_link==2);astep(&a);
    assert(a.command_link==2 && a.links[3].state==BT_ACL_ACCEPT_QUEUED);
    if(f.hci.command==BT_HCI_SENDING) astep(&a);
    cs(&f,HCI_OP_ACCEPT_CONN_REQ,1,0);astep(&a);assert(a.command_link==3);
    command(&f,&a,3,HCI_OP_ACCEPT_CONN_REQ);
    cs(&f,HCI_OP_ACCEPT_CONN_REQ,1,0);astep(&a);
    terminal(&f,&a.links[3],0,0x26);terminal(&f,&a.links[2],0,0x25);
    astep(&a);astep(&a);assert(a.links[2].state==BT_ACL_CONNECTED && a.links[3].state==BT_ACL_CONNECTED);
}
static void refusal_paths(void)
{
    struct fixture f;struct bt_acl a;uint8_t peer[]={1,2,3,4,5,6};uint64_t out=99;
    ready(&f,&a);assert(bt_acl_listen(&a,peer,5000,&out)==-1 && out==99);
    f.hci.identity.supported_commands[1]=0;assert(bt_acl_incoming(&a,2000)==-1);
    enable(&f,&a);assert(bt_acl_incoming(&a,2000)==-1);
    assert(bt_acl_listen(&a,f.hci.identity.address,5000,&out)==-1);
    assert(bt_acl_listen(&a,peer,5000,&a.next_link)==-1);
    listen_peer(&a,1);request(&f,peer,1);astep(&a);command(&f,&a,0,HCI_OP_ACCEPT_CONN_REQ);
    terminal(&f,&a.links[0],0,0x23);assert(bt_acl_step(&a,64,10000)==-1);
    assert(a.links[0].state==BT_ACL_ACCEPT_STATUS && a.links[0].handle==0xffff);
    enable(&f,&a);request(&f,peer,1);astep(&a);command(&f,&a,0,HCI_OP_REJECT_CONN_REQ);
    cs(&f,HCI_OP_REJECT_CONN_REQ,1,0);astep(&a);terminal(&f,&a.links[0],0,0x23);
    assert(bt_acl_step(&a,64,10000)==-1 && a.links[0].state==BT_ACL_REJECTING);
    enable(&f,&a);listen_peer(&a,1);request(&f,peer,1);astep(&a);
    command(&f,&a,0,HCI_OP_ACCEPT_CONN_REQ);cc(&f,HCI_OP_ACCEPT_CONN_REQ,1,1,0);
    assert(bt_acl_step(&a,64,10000)==-1); /* Complete success cannot replace Status. */
    enable(&f,&a);listen_peer(&a,1);request(&f,peer,1);astep(&a);
    command(&f,&a,0,HCI_OP_ACCEPT_CONN_REQ);cc(&f,HCI_OP_ACCEPT_CONN_REQ,1,1,1);astep(&a);
    assert(a.links[0].state==BT_ACL_CLOSED && a.links[0].status==1);
    enable(&f,&a);listen_peer(&a,1);accept_link(&f,&a,0,0x23);
    listen_peer(&a,7);request(&f,a.links[1].peer.address,1);astep(&a);
    command(&f,&a,1,HCI_OP_ACCEPT_CONN_REQ);cs(&f,HCI_OP_ACCEPT_CONN_REQ,1,0);astep(&a);
    terminal(&f,&a.links[1],0,0x23);assert(bt_acl_step(&a,64,10000)==-1 && a.links[1].handle==0xffff);
    enable(&f,&a);listen_peer(&a,1);request(&f,a.links[0].peer.address,1);astep(&a);
    command(&f,&a,0,HCI_OP_ACCEPT_CONN_REQ);cs(&f,HCI_OP_ACCEPT_CONN_REQ,1,0);astep(&a);
    struct hci_ev_conn_complete reserved={0,0x23,{{1,2,3,4,5,6}},1,2};
    event(&f,HCI_EV_CONN_COMPLETE,&reserved,sizeof(reserved));
    assert(bt_acl_step(&a,64,10000)==-1 && a.links[0].handle==0xffff && !a.links[0].encryption);
    enable(&f,&a);for(unsigned i=1;i<=4;++i)listen_peer(&a,i);
    peer[0]=9;request(&f,peer,1);size_t before=f.tn;
    assert(bt_acl_step(&a,64,10000)==-1 && f.tn==before && a.next_link==5);
    enable(&f,&a);listen_peer(&a,1);f.now=5000;before=f.accesses;
    assert(bt_acl_step(&a,64,10000)==-1 && f.accesses==before && a.links[0].state==BT_ACL_WAIT_REQUEST);
    enable(&f,&a);peer[0]=1;request(&f,peer,1);astep(&a);f.now=2001;
    assert(bt_acl_step(&a,64,10000)==-1 && a.links[0].state==BT_ACL_REJECT_STATUS);
    enable(&f,&a);listen_peer(&a,1);uint64_t existing=begin_connect(&f,&a,7);
    request(&f,a.links[existing-1].peer.address,1);assert(bt_acl_step(&a,64,10000)==-1);
}
static void synchronous_and_stale(void)
{
    struct fixture f;struct bt_acl a;enable(&f,&a);uint8_t peer[]={1,2,3,4,5,6};
    for(unsigned type=0;type<=2;type+=2) {
        request(&f,peer,type);astep(&a);const struct bt_h4_record *r=bt_acl_receive(&a);
        assert(r && r->storage.bytes[11]==type && a.next_link==1 &&
            bt_acl_receive_release(&a,r->serial)==1);
    }
    listen_peer(&a,1);
    /* Status acquired together with Request, before command transmission,
     * cannot satisfy the subsequently issued Accept. */
    request(&f,peer,1);cs(&f,HCI_OP_ACCEPT_CONN_REQ,1,0);
    astep(&a);astep(&a);assert(a.links[0].state==BT_ACL_ACCEPT_STATUS);
    const struct bt_h4_record *r=bt_acl_receive(&a);assert(r && !r->tag);
    assert(bt_acl_receive_release(&a,r->serial)==1);
    if(f.hci.command==BT_HCI_SENDING) astep(&a);
    cs(&f,HCI_OP_ACCEPT_CONN_REQ,1,0);astep(&a);
    terminal(&f,&a.links[0],0,0x23);astep(&a);assert(a.links[0].state==BT_ACL_CONNECTED);
    /* Duplicate success cannot assign the quarantined handle again. */
    terminal(&f,&a.links[0],0,0x23);assert(bt_acl_step(&a,64,10000)==-1);
    enable(&f,&a);request(&f,peer,3);assert(bt_acl_step(&a,64,10000)==-1);
    enable(&f,&a);event(&f,HCI_EV_CONN_REQUEST,peer,6);assert(bt_acl_step(&a,64,10000)==-1);
    /* Existing outbound happy path remains reachable with incoming enabled. */
    enable(&f,&a);uint64_t g=begin_connect(&f,&a,7);connected(&f,&a,g,0x23);
}
int main(void)
{
    incoming_data();rejected();interleaved();refusal_paths();synchronous_and_stale();
    puts("Incoming ACL UART/H4/HCI composition: copied peer admission, actual Accept/Reject wire commands, terminal correlation, ACL credits, security retention, bounded quarantine and fault paths passed.");
    return 0;
}
