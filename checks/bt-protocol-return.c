/* UART and source-layout fixtures are shared with the incoming-link check. */
#define main incoming_regression_main
#include "bt-acl-incoming.c"
#undef main
static void scan_off(struct fixture *f,struct bt_acl *a)
{
    assert(bt_hci_scan_disable(&f->hci,3000)==1);
    for(unsigned i=0;i<4 && f->hci.command!=BT_HCI_WAITING;++i)
        if(a)astep(a);else step(f,64);
    assert(f->hci.opcode==HCI_OP_WRITE_SCAN_ENABLE && f->hci.command==BT_HCI_WAITING);
    const uint8_t command_bytes[]={HCI_COMMAND_PKT,HCI_OP_WRITE_SCAN_ENABLE&255,HCI_OP_WRITE_SCAN_ENABLE>>8,1,0};
    assert(f->tn>=5 && !memcmp(f->tx+f->tn-5,command_bytes,5));
    assert(bt_hci_scan_disabled(&f->hci)==0);
    cc(f,HCI_OP_WRITE_SCAN_ENABLE,1,1,0);
    if(a)astep(a);else step(f,64);
    assert(bt_hci_scan_disabled(&f->hci)==1);
}
static void reset_finish(struct fixture *f)
{
    assert(bt_hci_stop(&f->hci,3000)==1);step(f,64);step(f,64);
    assert(f->hci.command==BT_HCI_WAITING && f->hci.opcode==HCI_OP_RESET);
    assert(bt_hci_stopped(&f->hci)==0);
    cc(f,HCI_OP_RESET,1,1,0);step(f,64);
    assert(bt_hci_stopped(&f->hci)==1);
    unsigned io=f->accesses;
    for(unsigned i=0;i<4;++i)step(f,64);
    assert(f->accesses==io);
    const uint8_t peer[6]={1,2,3,4,5,6};
    assert(bt_hci_request(&f->hci,HCI_OP_CREATE_CONN_CANCEL,peer,6,4000)==-1);
}
static void startup_return(void)
{
    struct fixture f;init(&f);scan_off(&f,0);
    assert(f.hci.stage==0 && f.hci.identity.valid==0);reset_finish(&f);
    init(&f);send(&f,HCI_OP_RESET);uint64_t original=f.hci.command_deadline;
    assert(bt_hci_scan_disable(&f.hci,3000)==1);
    assert(bt_hci_scan_disable(&f.hci,8000)==1 && f.hci.return_deadline==3000);
    assert(f.hci.command_deadline==original);
    cc(&f,HCI_OP_RESET,1,1,0);step(&f,64);
    assert(f.hci.stage==1 && f.hci.opcode==HCI_OP_WRITE_SCAN_ENABLE);
    scan_off(&f,0);reset_finish(&f);
    config_identify(&f);struct bt_hci_incoming_config c=configuration(&f);
    assert(bt_hci_configure_incoming(&f.hci,&c,9000)==1);
    step(&f,64);step(&f,64);unsigned op=f.hci.opcode;
    assert(bt_hci_scan_disable(&f.hci,3000)==1);cc(&f,op,1,1,0);step(&f,64);
    assert(f.hci.configuration_stage==1 && !f.hci.configured);
    scan_off(&f,0);reset_finish(&f);
}
static void terminal_faults(void)
{
    for(unsigned which=0;which<4;++which){
        struct fixture f;init(&f);initialize_all(&f);scan_off(&f,0);
        assert(bt_hci_stop(&f.hci,3000)==1);step(&f,64);step(&f,64);
        if(which==0)cc(&f,HCI_OP_RESET,1,1,1);
        if(which==1)cs(&f,HCI_OP_RESET,1,0);
        if(which==2)cc(&f,HCI_OP_RESET,1,2,0);
        if(which==3)f.now=f.hci.command_deadline;
        assert(bt_hci_step(&f.hci,64,3000)==-1 && bt_hci_stopped(&f.hci)==-1);
        unsigned io=f.accesses;assert(bt_hci_step(&f.hci,64,3000)==-1 && f.accesses==io);
    }
    struct fixture f;init(&f);send(&f,HCI_OP_RESET);uint64_t original=f.hci.command_deadline;
    assert(bt_hci_scan_disable(&f.hci,3000)==1);f.now=original;
    assert(bt_hci_step(&f.hci,64,3000)==-1 && f.hci.stage==0);
    init(&f);initialize_all(&f);scan_off(&f,0);
    const uint8_t unknown[]={0x44};event(&f,0xfe,unknown,1);step(&f,64);
    const struct bt_h4_record *r=bt_hci_next(&f.hci);assert(r);
    size_t count=f.tn;assert(bt_hci_stop(&f.hci,3000)==1);step(&f,64);
    assert(f.tn==count && bt_hci_stopped(&f.hci)==0);
    f.now=3000;assert(bt_hci_step(&f.hci,64,3000)==-1);
    assert(bt_hci_next(&f.hci)==r && r->owner==BT_H4_OWNED);
}
static void stale_and_partial(void)
{
    struct fixture f;init(&f);initialize_all(&f);scan_off(&f,0);
    /* Acquire an old Reset completion before a new Reset is transmitted. It
     * must retain its old acquisition tag and cannot complete the new command. */
    cc(&f,HCI_OP_RESET,1,1,0);step(&f,64);
    const struct bt_h4_record *old=bt_hci_next(&f.hci);assert(old && old->tag==0);
    assert(bt_hci_stop(&f.hci,3000)==1);size_t transmitted=f.tn;step(&f,64);
    assert(f.tn==transmitted && f.hci.command==BT_HCI_IDLE);
    assert(bt_hci_release(&f.hci,old->serial)==1);step(&f,64);step(&f,64);
    assert(bt_hci_stopped(&f.hci)==0 && f.hci.command==BT_HCI_WAITING);
    cc(&f,HCI_OP_RESET,1,1,0);step(&f,64);assert(bt_hci_stopped(&f.hci)==1);
    /* The response first byte is acquired while Scan Enable is SENDING. The
     * remainder arriving after final TX cannot borrow the new command tag. */
    init(&f);initialize_all(&f);assert(bt_hci_scan_disable(&f.hci,3000)==1);step(&f,64);
    const uint8_t first[]={HCI_EVENT_PKT};bytes(&f,first,sizeof(first));step(&f,1);
    uint64_t partial=f.hci.transport.partial_deadline;
    const uint8_t rest[]={HCI_EV_CMD_COMPLETE,4,1,HCI_OP_WRITE_SCAN_ENABLE&255,HCI_OP_WRITE_SCAN_ENABLE>>8,0};
    bytes(&f,rest,sizeof(rest));step(&f,64);
    assert(bt_hci_scan_disabled(&f.hci)==0);
    old=bt_hci_next(&f.hci);assert(old && old->tag==0);
    assert(bt_hci_release(&f.hci,old->serial)==1);step(&f,64);
    cc(&f,HCI_OP_WRITE_SCAN_ENABLE,1,1,0);step(&f,64);assert(bt_hci_scan_disabled(&f.hci)==1);
    /* A partial frame retains its original 100 us bound at return. */
    init(&f);initialize_all(&f);bytes(&f,first,sizeof(first));step(&f,1);
    partial=f.hci.transport.partial_deadline;assert(bt_hci_stop(&f.hci,3000)==1);
    f.now=partial;assert(bt_hci_step(&f.hci,64,3000)==-1 && f.hci.transport.assembling<f.hci.transport.count);
}
static void cancel_complete(struct fixture *f,const struct bt_acl_link *l,unsigned status,unsigned wrong_peer)
{
    uint8_t p[10]={1,HCI_OP_CREATE_CONN_CANCEL&255,HCI_OP_CREATE_CONN_CANCEL>>8,(uint8_t)status};
    memcpy(p+4,l->peer.address,6);p[4]+=(uint8_t)wrong_peer;
    event(f,HCI_EV_CMD_COMPLETE,p,sizeof(p));
}
static void cancel_waiting(struct fixture *f,struct bt_acl *a)
{
    ready(f,a);begin_connect(f,a,1);
    assert(bt_acl_stop(a,3000)==1);cs(f,HCI_OP_CREATE_CONN,1,0);astep(a);
    assert(a->links[0].state==BT_ACL_CANCEL_STATUS);
    size_t tx=f->tn;astep(a);
    struct hci_cp_create_conn_cancel ref={{{1,2,3,4,5,6}}};
    const uint8_t header[]={HCI_COMMAND_PKT,HCI_OP_CREATE_CONN_CANCEL&255,HCI_OP_CREATE_CONN_CANCEL>>8,sizeof(ref)};
    assert(f->tn==tx+sizeof(header)+sizeof(ref));
    assert(!memcmp(f->tx+tx,header,sizeof(header)) && !memcmp(f->tx+tx+4,&ref,sizeof(ref)));
}
static void cancellation(void)
{
    struct fixture f;struct bt_acl a;cancel_waiting(&f,&a);
    cancel_complete(&f,&a.links[0],0,0);astep(&a);
    assert(a.links[0].state==BT_ACL_CANCELLING && bt_acl_quiescent(&a)==0);
    terminal(&f,&a.links[0],2,0);astep(&a);
    assert(bt_acl_quiescent(&a)==1);scan_off(&f,&a);reset_finish(&f);
    cancel_waiting(&f,&a);terminal(&f,&a.links[0],0,0x23);astep(&a);
    assert(a.links[0].state==BT_ACL_CANCEL_STATUS && a.links[0].handle_valid);
    cancel_complete(&f,&a.links[0],0x0b,0);astep(&a);astep(&a);
    assert(f.hci.opcode==HCI_OP_DISCONNECT && bt_acl_quiescent(&a)==0);
    cs(&f,HCI_OP_DISCONNECT,1,0);astep(&a);disconnect_event(&f,0x23);astep(&a);
    assert(bt_acl_quiescent(&a)==1);
    for(unsigned which=0;which<4;++which){
        cancel_waiting(&f,&a);
        if(which==0)cancel_complete(&f,&a.links[0],0,1);
        if(which==1)cs(&f,HCI_OP_CREATE_CONN_CANCEL,1,0);
        if(which==2){cancel_complete(&f,&a.links[0],0,0);astep(&a);terminal(&f,&a.links[0],0,0x23);}
        if(which==3){cancel_complete(&f,&a.links[0],0,0);astep(&a);f.now=3000;}
        assert(bt_acl_step(&a,64,3000)==-1 && bt_acl_quiescent(&a)==-1);
    }
}
static void packet_retirement(void)
{
    struct fixture f;struct bt_acl a;ready(&f,&a);
    uint64_t gen=begin_connect(&f,&a,1);connected(&f,&a,gen,0x23);
    const uint8_t payload[]={1,2,3};uint64_t serial=queue(&a,gen,payload,sizeof(payload));
    sent(&f,&a,payload,sizeof(payload),0x23);
    const uint8_t second[]={4,5,6};uint64_t unsent=queue(&a,gen,second,sizeof(second));
    assert(bt_acl_stop(&a,3000)==1 && bt_acl_quiescent(&a)==0);
    assert(bt_acl_submit(&a,gen,2,second,sizeof(second),3000,&unsent)==-1);
    astep(&a);astep(&a);cs(&f,HCI_OP_DISCONNECT,1,0);astep(&a);
    assert(bt_acl_quiescent(&a)==0 && a.packets[0].bytes==payload);
    disconnect_event(&f,0x23);astep(&a);
    assert(bt_acl_quiescent(&a)==1 && a.packets[0].state==BT_ACL_FLUSHED);
    assert(a.packets[0].bytes==payload && a.packets[1].state==BT_ACL_NOT_SENT);
    assert(bt_acl_release(&a,serial)==1 && bt_acl_release(&a,unsent)==1);
    ready(&f,&a);gen=begin_connect(&f,&a,1);connected(&f,&a,gen,0x23);
    queue(&a,gen,payload,sizeof(payload));astep(&a);f.room=2;astep(&a);
    assert(a.packets[0].state==BT_ACL_PARTIAL && f.hci.transport.tx_offset==2);
    assert(bt_acl_stop(&a,3000)==1);uint64_t original=f.hci.transport.tx_deadline;
    f.now=original;assert(bt_acl_step(&a,64,3000)==-1);
    assert(a.packets[0].state==BT_ACL_PARTIAL && a.packets[0].bytes==payload);
}
static void incoming_stop(void)
{
    struct fixture f;struct bt_acl a;enable(&f,&a);uint64_t gen=listen_peer(&a,1);
    assert(bt_acl_close_admission(&a,3000)==1 && a.links[gen-1].state==BT_ACL_CLOSED);
    uint8_t address[]={1,2,3,4,5,6};request(&f,address,1);astep(&a);
    assert(a.links[gen-1].state==BT_ACL_REJECT_STATUS);
    command(&f,&a,0,HCI_OP_REJECT_CONN_REQ);cs(&f,HCI_OP_REJECT_CONN_REQ,1,0);astep(&a);
    assert(bt_acl_stop(&a,3000)==1 && bt_acl_quiescent(&a)==0);
    terminal(&f,&a.links[0],0x0e,0);astep(&a);assert(bt_acl_quiescent(&a)==1);
    scan_off(&f,&a);reset_finish(&f);
    ready(&f,&a);assert(bt_acl_stop(&a,3000)==1);request(&f,address,1);
    assert(bt_acl_step(&a,64,3000)==-1 && bt_acl_quiescent(&a)==-1);
}
int main(void)
{
    (void)incoming_regression_main;
    startup_return();terminal_faults();stale_and_partial();cancellation();packet_retirement();incoming_stop();
    puts("Bluetooth return: startup halt, scan-disable, terminal Reset, two-event cancellation, connection race, retained packets, incoming rejection and sticky deadlines passed.");
    return 0;
}
