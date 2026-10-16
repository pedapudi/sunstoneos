#include "bt_hci.h"
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
static void command_cases(void)
{
    struct fixture f;init(&f);step(&f,64);
    step(&f,2);assert(f.hci.command==BT_HCI_SENDING && f.tn==2);
    /* A stale matching response was acquired before final TX acceptance. */
    cc(&f,HCI_OP_RESET,1,1,0);step(&f,64);
    assert(f.hci.stage==0 && f.hci.command==BT_HCI_SENDING);
    const struct bt_h4_record *r=bt_hci_next(&f.hci);assert(r && r->tag==0);
    uint64_t serial=r->serial;assert(bt_hci_release(&f.hci,serial)==1);
    assert(bt_hci_release(&f.hci,serial)==-1);
    step(&f,64);assert(f.hci.command==BT_HCI_WAITING && f.tn==4);
    /* A fragmented proper response completes, but zero credits prevent the
     * next command until an independent opcode-zero credit update arrives. */
    cc(&f,HCI_OP_RESET,0,1,0);
    for(unsigned i=0;i<7;++i)step(&f,1);
    assert(f.hci.stage==1 && f.hci.command==BT_HCI_IDLE && f.hci.credits==0);
    size_t sent=f.tn;step(&f,64);assert(f.tn==sent);
    cc(&f,0,1,0,0);step(&f,64);send(&f,HCI_OP_READ_LOCAL_VERSION);
    cs(&f,HCI_OP_READ_LOCAL_VERSION,1,0);step(&f,64);
    assert(f.hci.command_accepted && f.hci.stage==1 && f.hci.command==BT_HCI_WAITING);
    cc(&f,HCI_OP_RESET,5,1,0);step(&f,64);
    assert(f.hci.stage==1 && bt_hci_next(&f.hci));
    assert(bt_hci_release(&f.hci,bt_hci_next(&f.hci)->serial)==1);
    cc(&f,HCI_OP_READ_LOCAL_VERSION,1,sizeof(struct hci_rp_read_local_version),0);step(&f,64);
    assert(f.hci.stage==2 && f.hci.identity.version[7]==0x38);
    init(&f);initialize_all(&f);
}
static void retained_packets(void)
{
    struct fixture f;init(&f);initialize_all(&f);
    const uint8_t packets[]={
        HCI_ACLDATA_PKT,0x23,0x21,3,0,0xaa,0xbb,0xcc,
        HCI_SCODATA_PKT,0x23,0x11,2,0xdd,0xee,
        HCI_ISODATA_PKT,0x23,0x21,4,0,1,0,0,0,
        HCI_EVENT_PKT,HCI_EV_DISCONN_COMPLETE,4,0,0x23,1,0x13};
    bytes(&f,packets,sizeof(packets));step(&f,64);step(&f,64);step(&f,64);
    assert(f.hci.transport.staged>f.hci.transport.stage_offset);
    unsigned accesses=f.accesses;step(&f,64);assert(f.accesses==accesses);
    const struct bt_h4_record *r=bt_hci_next(&f.hci);
    assert(r && r->type==2 && r->length==7 && !memcmp(r->storage.bytes,packets+1,7));
    uint64_t serial=r->serial;assert(bt_hci_release(&f.hci,serial)==1);
    step(&f,64);r=bt_hci_next(&f.hci);
    assert(r && r->type==3 && r->length==5 && !memcmp(r->storage.bytes,packets+9,5));
    assert(bt_hci_release(&f.hci,r->serial)==1);r=bt_hci_next(&f.hci);
    assert(r && r->type==5 && r->length==8 && !memcmp(r->storage.bytes,packets+15,8));
    assert(bt_hci_release(&f.hci,r->serial)==1);r=bt_hci_next(&f.hci);
    assert(r && r->type==4 && r->storage.bytes[0]==HCI_EV_DISCONN_COMPLETE);
    f.current=0;accesses=f.accesses;
    assert(bt_hci_step(&f.hci,64,10000)==-1 && f.accesses==accesses && f.hci.state==BT_HCI_FAULT);
    assert(bt_hci_next(&f.hci)==r && bt_hci_release(&f.hci,r->serial)==-1);
}
static void failure_cases(void)
{
    struct fixture f;init(&f);send(&f,HCI_OP_RESET);
    cs(&f,HCI_OP_RESET,1,0x0c);
    assert(bt_hci_step(&f.hci,64,10000)==-1 && f.hci.last_status==0x0c && bt_hci_next(&f.hci));
    init(&f);const uint8_t partial[]={HCI_ACLDATA_PKT,1,0,10,0};bytes(&f,partial,sizeof(partial));step(&f,64);
    assert(f.hci.transport.assembling<f.hci.transport.count);f.now=101;
    assert(bt_hci_step(&f.hci,64,10000)==-1);
    init(&f);const uint8_t large[]={HCI_ACLDATA_PKT,1,0,0xff,0xff};bytes(&f,large,sizeof(large));
    assert(bt_hci_step(&f.hci,64,10000)==-1 && f.hci.transport.records[0].length==4);
    init(&f);uint8_t badtype=1;bytes(&f,&badtype,1);
    assert(bt_hci_step(&f.hci,64,10000)==-1 && f.hci.transport.staged==1);
    init(&f);uint8_t validtype=4;bytes(&f,&validtype,1);f.error=4;
    assert(bt_hci_step(&f.hci,64,10000)==-1 && f.uart.errors==4 && f.hci.transport.staged==1);
    unsigned n=f.accesses;
    assert(bt_hci_step(&f.hci,64,10000)==-1 && f.accesses==n);
    init(&f);send(&f,HCI_OP_RESET);cs(&f,0,0,0);step(&f,64);
    assert(f.hci.command==BT_HCI_WAITING && f.hci.credits==0);
    f.now=501;assert(bt_hci_step(&f.hci,64,10000)==-1);
    cc(&f,HCI_OP_RESET,1,1,0);n=f.accesses;
    assert(bt_hci_step(&f.hci,64,10000)==-1 && f.accesses==n);
    init(&f);step(&f,64);f.uncertain=1;
    assert(bt_hci_step(&f.hci,64,10000)==-1 && f.tn==1 && f.hci.transport.tx_offset==0);
    n=f.accesses;assert(bt_hci_step(&f.hci,64,10000)==-1 && f.accesses==n);
    init(&f);f.now=0;assert(bt_hci_step(&f.hci,64,10000)==-1);
    init(&f);uint8_t malformed[2]={1,2};event(&f,HCI_EV_CMD_STATUS,malformed,2);
    assert(bt_hci_step(&f.hci,64,10000)==-1 && bt_hci_next(&f.hci));
}
static void drain_before_tx(void)
{
    struct fixture f;init(&f);step(&f,64);
    const uint8_t payload[]={1,2,3,4};
    event(&f,0xff,payload,4);event(&f,0xff,payload,4);cc(&f,HCI_OP_RESET,1,1,0);
    /* The stale completion begins behind several receive budgets. No command
     * byte may transmit while any preexisting bytes remain unclassified. */
    for(unsigned i=0;i<12;++i) {step(&f,4);assert(f.tn==0);}
    assert(f.ri==f.rn && f.hci.command==BT_HCI_SENDING && f.hci.stage==0);
    const struct bt_h4_record *r=bt_hci_next(&f.hci);assert(r && r->storage.bytes[0]==0xff);
    assert(bt_hci_release(&f.hci,r->serial)==1);
    step(&f,64);assert(f.tn==4 && f.hci.command==BT_HCI_WAITING && f.hci.stage==0);
    r=bt_hci_next(&f.hci);assert(r && r->storage.bytes[0]==0xff);
    assert(bt_hci_release(&f.hci,r->serial)==1);
    r=bt_hci_next(&f.hci);assert(r && r->storage.bytes[0]==HCI_EV_CMD_COMPLETE && r->tag==0);
    assert(bt_hci_release(&f.hci,r->serial)==1);
    cc(&f,HCI_OP_RESET,1,1,0);step(&f,64);assert(f.hci.stage==1);
    /* A valid controller reply produced immediately after the final TX write
     * stays unread until WAITING and is accepted on the next worker poll. */
    init(&f);f.immediate_reset_reply=1;send(&f,HCI_OP_RESET);
    assert(f.ri==0 && f.rn==7 && f.hci.stage==0);
    step(&f,64);assert(f.hci.stage==1);
    puts("H4 pre-transmit drain preserves FIFO-tail ordering and immediate valid responses.");
}
int main(int argc, char **argv)
{
    if(argc>1 && !strcmp(argv[1], "--drain-before-tx")) {drain_before_tx();return 0;}
    assert(sizeof(struct hci_command_hdr)==3 && sizeof(struct hci_event_hdr)==2 &&
           sizeof(struct hci_acl_hdr)==4 && sizeof(struct hci_sco_hdr)==3 && sizeof(struct hci_iso_hdr)==4);
    command_cases();retained_packets();failure_cases();
    puts("UART-composed H4 framing, HCI credits/initialization, retained packets and fault model passed.");
    return 0;
}
