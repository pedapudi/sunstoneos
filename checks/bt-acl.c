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
static void scheduling(void)
{
    struct fixture f;struct bt_acl a;ready(&f,&a);
    uint64_t l1=begin_connect(&f,&a,1);connected(&f,&a,l1,0x23);
    uint64_t l2=begin_connect(&f,&a,7);connected(&f,&a,l2,0x24);
    const uint8_t one[]={1,2,3},two[]={4,5,6},three[]={7,8,9};
    uint64_t t1=queue(&a,l1,one,sizeof(one));sent(&f,&a,one,sizeof(one),0x23);
    uint64_t t2=queue(&a,l2,two,sizeof(two));sent(&f,&a,two,sizeof(two),0x24);
    assert(a.credits==0);uint64_t t3=queue(&a,l1,three,sizeof(three));
    size_t tn=f.tn;astep(&a);assert(f.tn==tn && a.active_tx==BT_ACL_TX_SLOTS);
    /* RX data keeps its backing while a later multi-handle credit event runs. */
    const uint8_t rx[]={2,0x23,0x20,2,0,0xaa,0xbb};bytes(&f,rx,sizeof(rx));astep(&a);
    const struct bt_h4_record *r=bt_acl_receive(&a);assert(r && r->type==2 && !memcmp(r->storage.bytes,rx+1,6));
    uint64_t rs=r->serial;
    struct hci_comp_pkts_info credits[]={{0x24,1},{0x23,1}};completed(&f,credits,2);astep(&a);
    assert(a.credits==1 && a.active_tx<BT_ACL_TX_SLOTS);
    assert(bt_acl_result(&a)->serial==t1 && bt_acl_release(&a,t1)==1);
    assert(bt_acl_result(&a)->serial==t2 && bt_acl_release(&a,t2)==1);
    assert(bt_acl_receive(&a)->serial==rs && bt_acl_receive_release(&a,rs)==1);
    astep(&a);astep(&a);assert(a.active_tx==BT_ACL_TX_SLOTS);
    assert(bt_acl_disconnect(&a,l1,6000)==1);astep(&a);tn=f.tn;astep(&a);
    struct hci_cp_disconnect ref={0x23,0x13};
    assert(f.tn==tn+7 && f.tx[tn]==1 && f.tx[tn+1]==6 && f.tx[tn+2]==4 && f.tx[tn+3]==3);
    assert(!memcmp(f.tx+tn+4,&ref,sizeof(ref)));
    cs(&f,HCI_OP_DISCONNECT,1,0);astep(&a);assert(a.links[l1-1].state==BT_ACL_DISCONNECTING);
    assert(a.credits==1 && !bt_acl_result(&a));disconnect_event(&f,0x23);astep(&a);
    assert(a.credits==2 && a.links[l1-1].state==BT_ACL_CLOSED);
    assert(bt_acl_result(&a)->serial==t3 && bt_acl_result(&a)->state==BT_ACL_FLUSHED &&
           bt_acl_result(&a)->uncertain && bt_acl_release(&a,t3)==1);
    /* Reusing a retired handle never redirects later completions. */
    uint64_t l3=begin_connect(&f,&a,8);cs(&f,HCI_OP_CREATE_CONN,1,0);astep(&a);
    struct hci_ev_conn_complete reused={0,0x23,{{8,2,3,4,5,6}},1,0};
    event(&f,HCI_EV_CONN_COMPLETE,&reused,sizeof(reused));
    assert(bt_acl_step(&a,64,10000)==-1 && a.links[l3-1].state==BT_ACL_CONNECTING && a.credits==2);
    assert(bt_acl_receive(&a) && bt_acl_receive_release(&a,bt_acl_receive(&a)->serial)==-1);
}
static void atomic_counts(void)
{
    struct fixture f;struct bt_acl a;ready(&f,&a);
    uint64_t l1=begin_connect(&f,&a,1);connected(&f,&a,l1,0x23);
    uint64_t l2=begin_connect(&f,&a,7);connected(&f,&a,l2,0x24);
    const uint8_t one[]={1},two[]={2};queue(&a,l1,one,1);sent(&f,&a,one,1,0x23);
    queue(&a,l2,two,1);sent(&f,&a,two,1,0x24);
    struct bt_acl_packet before[BT_ACL_TX_SLOTS];memcpy(before,a.packets,sizeof(before));
    struct hci_comp_pkts_info bad[]={{0x23,1},{0x24,2}};completed(&f,bad,2);
    assert(bt_acl_step(&a,64,10000)==-1 && !a.credits && !memcmp(before,a.packets,sizeof(before)));
    assert(bt_acl_receive(&a) && bt_acl_release(&a,1)==-1);
}
static void retained_failures(void)
{
    struct fixture f;struct bt_acl a;const uint8_t data[]={1,2,3};ready(&f,&a);
    uint64_t l=begin_connect(&f,&a,1);connected(&f,&a,l,0x23);queue(&a,l,data,3);
    astep(&a);astep(&a);assert(a.packets[0].state==BT_ACL_PARTIAL && f.hci.transport.tx_offset==5);
    disconnect_event(&f,0x23);assert(bt_acl_step(&a,64,10000)==-1);
    assert(a.packets[0].state==BT_ACL_PARTIAL && a.credits==1 && a.links[0].state==BT_ACL_CONNECTED);
    assert(f.hci.transport.tx_offset==5 && f.hci.transport.tx_payload==data);
    ready(&f,&a);l=begin_connect(&f,&a,1);connected(&f,&a,l,0x23);queue(&a,l,data,3);sent(&f,&a,data,3,0x23);
    const uint8_t h[]={0x23,0};event(&f,0x11,h,sizeof(h));astep(&a);
    assert(a.links[0].flush_seen && a.packets[0].uncertain && a.credits==1);
    uint64_t untouched=99;assert(bt_acl_submit(&a,l,2,data,3,5000,&untouched)==-1 && untouched==99);
    struct hci_comp_pkts_info c={0x23,1};completed(&f,&c,1);astep(&a);
    assert(a.credits==2 && bt_acl_result(&a)->uncertain && bt_acl_release(&a,1)==1);
    disconnect_event(&f,0x23);astep(&a);assert(a.links[0].state==BT_ACL_CLOSED);
    ready(&f,&a);l=begin_connect(&f,&a,1);connected(&f,&a,l,0x23);queue(&a,l,data,3);sent(&f,&a,data,3,0x23);
    f.now=5000;assert(bt_acl_step(&a,64,10000)==-1 && a.packets[0].state==BT_ACL_SENT && a.credits==1);
}
static void closed_handle_count(void)
{
    struct fixture f;struct bt_acl a;ready(&f,&a);
    uint64_t l=begin_connect(&f,&a,1);connected(&f,&a,l,0x23);
    const uint8_t payload[]={1};queue(&a,l,payload,1);sent(&f,&a,payload,1,0x23);
    disconnect_event(&f,0x23);astep(&a);
    assert(a.credits==2 && a.packets[0].state==BT_ACL_FLUSHED);
    struct hci_comp_pkts_info stale={0x23,1};completed(&f,&stale,1);
    assert(bt_acl_step(&a,64,10000)==-1 && a.credits==2 && a.packets[0].state==BT_ACL_FLUSHED);
}
static void command_ownership(void)
{
    struct fixture f;struct bt_acl a;ready(&f,&a);
    uint64_t l=begin_connect(&f,&a,1);
    /* A different command's status supplies neither link acceptance nor credit.
     * No worker owns it, so the link worker frees its receive slot. */
    cs(&f,HCI_OP_DISCONNECT,9,0);astep(&a);
    assert(!f.hci.credits && a.links[0].state==BT_ACL_CONNECT_STATUS && !bt_acl_receive(&a));
    cs(&f,HCI_OP_CREATE_CONN,1,0x0c);astep(&a);
    assert(a.links[0].state==BT_ACL_CLOSED && a.links[0].status==0x0c && !bt_acl_receive(&a));
    /* That peer may retake only this slot; checks/bt-acl-incoming.c covers it. */
    l=begin_connect(&f,&a,2);cc(&f,HCI_OP_CREATE_CONN,1,1,1);astep(&a);
    assert(a.links[l-1].state==BT_ACL_CLOSED && a.links[l-1].status==1);
    l=begin_connect(&f,&a,3);connected(&f,&a,l,0x33);
    assert(bt_acl_disconnect(&a,l,6000)==1);astep(&a);astep(&a);
    cs(&f,HCI_OP_DISCONNECT,1,0x0c);astep(&a);
    assert(a.links[l-1].state==BT_ACL_CONNECTED);
    disconnect_event(&f,0x33);astep(&a);
    /* A peer that once held a handle stays retained after disconnect. */
    struct bt_acl_peer closed={{3,2,3,4,5,6},1,0,0};uint64_t unchanged=99;
    assert(bt_acl_connect(&a,&closed,5000,&unchanged)==-1 && unchanged==99);
    l=begin_connect(&f,&a,4);cs(&f,HCI_OP_CREATE_CONN,1,0x0c);astep(&a);
    struct bt_acl_peer peer={{5,2,3,4,5,6},1,0,0};uint64_t output=123;
    assert(bt_acl_connect(&a,&peer,5000,&output)==0 && output==123 && l==4);
}
/* Events no owner consumes free their receive slot after exact validation.
 * Other unknown events stay owned, and a malformed known one faults. */
static void unowned_events(void)
{
    struct fixture f;struct bt_acl a;ready(&f,&a);
    uint64_t l=begin_connect(&f,&a,1);connected(&f,&a,l,0x23);
    const uint8_t slots[]={0x23,0,5},lsto[]={0x23,0,0x80,0x0c},role[]={0,1,2,3,4,5,6,1},
        mode[]={0,0x23,0,2,0x20,0},psrm[]={1,2,3,4,5,6,1},vendor[]={0x42},
        features[]={0,0x23,0,1,2,3,4,5,6,7,8},failed[]={0x04,0x24,0,9,2,3,4,5,6,1,0},
        request[]={9,2,3,4,5,6,4,4,0x24,1},sync[]={0x10,0,0,9,2,3,4,5,6,2,0,0,0,0,0,0,2};
    const struct {unsigned code;const uint8_t *p;unsigned n;} released[]={{0x1b,slots,3},{0x38,lsto,4},
        {0x12,role,8},{0x14,mode,6},{0x20,psrm,7},{0xff,vendor,1},{0xff,vendor,0},{0x0b,features,11},
        {HCI_EV_CONN_COMPLETE,failed,11},{0x04,request,10},{0x2c,sync,17}};
    for(unsigned i=0;i<sizeof(released)/sizeof(released[0]);++i){
        event(&f,released[i].code,released[i].p,released[i].n);astep(&a);
        assert(!bt_acl_receive(&a) && a.links[0].state==BT_ACL_CONNECTED && a.next_link==2);
    }
    const uint8_t overflow[]={1};event(&f,0x1a,overflow,1);astep(&a);
    const struct bt_h4_record *r=bt_acl_receive(&a);
    assert(r && r->storage.bytes[0]==0x1a && bt_acl_receive_release(&a,r->serial)==1);
    event(&f,0x38,lsto,3);assert(bt_acl_step(&a,64,10000)==-1 && a.links[0].state==BT_ACL_CONNECTED);
}
int main(void)
{
    scheduling();atomic_counts();retained_failures();closed_handle_count();command_ownership();unowned_events();
    puts("Source-layout ACL/link commands, bounded UART composition, exact credits, retained disconnect/flush/fault, unowned-event release and lifetime limits passed.");
    return 0;
}
