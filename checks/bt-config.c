/* SPDX-License-Identifier: Apache-2.0 */
/* Reuse setup and wire helpers without executing previous ACL scenarios. */
#define main bt_incoming_previous_scenarios
#include "bt-acl-incoming.c"
#undef main

static void configured_handoff(unsigned partial)
{
    struct fixture f;struct bt_acl a={0},second={0};
    config_identify(&f);config_start(&f);config_through(&f,7);config_send(&f,7);
    const uint8_t peer[]={1,2,3,4,5,6};
    if(partial) {
        config_reply(&f,7,0);const uint8_t start[]={4,4,10,1};bytes(&f,start,sizeof(start));step(&f,64);
        assert(f.hci.transport.assembling<f.hci.transport.count);
    } else {
        request(&f,peer,1);config_reply(&f,7,0);step(&f,64);
        assert(f.hci.state==BT_HCI_CONFIGURING && bt_hci_next(&f.hci));step(&f,64);
    }
    assert(f.hci.state==BT_HCI_IDENTIFIED && f.hci.configured && f.hci.command_tag==14);
    struct bt_hci copied=f.hci;unsigned before=f.accesses;
    assert(bt_hci_step(&copied,64,10000)==-1 && f.accesses==before);
    assert(bt_acl_init(&a,&copied)==-1);
    assert(bt_acl_init(&a,&f.hci)==1 && f.hci.acl_owner==&a);
    assert(bt_acl_init(&second,&f.hci)==-1 && !second.hci);
    struct bt_acl duplicate=a;
    assert(bt_acl_step(&duplicate,64,10000)==-1 && f.accesses==before);
    uint64_t actual=(uint64_t)f.hci.incoming_config.accept_timeout_slots*625;
    assert(bt_acl_incoming(&a,actual-1000)==-1 && !a.incoming_timeout_us);
    assert(bt_acl_incoming(&a,2000)==1);uint64_t generation=0;
    assert(bt_acl_listen(&a,peer,5000,&generation)==1 && generation==1);
    if(partial) {
        struct hci_ev_conn_request req={{{1,2,3,4,5,6}},{4,4,0x24},1};
        bytes(&f,(const uint8_t *)&req+1,sizeof(req)-1);
    }
    astep(&a);command(&f,&a,0,HCI_OP_ACCEPT_CONN_REQ);
    cs(&f,HCI_OP_ACCEPT_CONN_REQ,1,0);astep(&a);
    terminal(&f,&a.links[0],0,0x23);astep(&a);
    assert(a.links[0].state==BT_ACL_CONNECTED && !a.links[0].encryption && a.links[0].request_serial);
    const uint8_t payload[]={1,2,3};queue(&a,generation,payload,3);sent(&f,&a,payload,3,0x23);
    struct hci_comp_pkts_info credit={0x23,1};completed(&f,&credit,1);astep(&a);assert(a.credits==2);
    event(&f,0x17,peer,6);astep(&a);const struct bt_h4_record *key=bt_acl_receive(&a);
    assert(key && key->storage.bytes[0]==0x17 && !a.links[0].encryption);
    struct bt_hci_incoming_config c=configuration(&f);
    assert(bt_hci_configure_incoming(&f.hci,&c,9000)==-1);
    config_service_current=0;before=f.accesses;
    assert(bt_acl_step(&a,64,10000)==-1 && f.accesses==before && f.hci.acl_owner==&a);
}
static void unreserved_handoff(void)
{
    struct fixture f;struct bt_acl a={0};config_identify(&f);config_start(&f);
    config_through(&f,7);config_send(&f,7);
    const uint8_t unknown[]={9,2,3,4,5,6};request(&f,unknown,1);
    config_reply(&f,7,0);step(&f,64);step(&f,64);
    assert(bt_acl_init(&a,&f.hci)==1 && bt_acl_incoming(&a,2000)==1);
    astep(&a);assert(a.links[0].state==BT_ACL_REJECT_STATUS && a.links[0].reason==0x0e);
    command(&f,&a,0,HCI_OP_REJECT_CONN_REQ);cs(&f,HCI_OP_REJECT_CONN_REQ,1,0);astep(&a);
    /* The matched rejection frees its slot. */
    terminal(&f,&a.links[0],0x16,0);astep(&a);assert(a.links[0].state==BT_ACL_UNUSED && !a.links[0].generation);
}
static void refusal_and_deadlines(void)
{
    struct fixture f;struct bt_acl a={0};config_identify(&f);
    struct bt_hci_incoming_config c=configuration(&f),bad=c;
    unsigned before=f.accesses;
    bad.page_interval_slots=0x801;assert(bt_hci_configure_incoming(&f.hci,&bad,9000)==-1);
    bad=c;bad.page_window_slots=0x801;assert(bt_hci_configure_incoming(&f.hci,&bad,9000)==-1);
    bad=c;bad.accept_timeout_slots=0x9f;assert(bt_hci_configure_incoming(&f.hci,&bad,9000)==-1);
    bad=c;bad.receive_latency_us=(uint64_t)c.accept_timeout_slots*625;
    assert(bt_hci_configure_incoming(&f.hci,&bad,9000)==-1);
    assert(f.accesses==before && !f.hci.configuration_deadline && !f.hci.configured);
    config_service_current=0;assert(bt_hci_configure_incoming(&f.hci,&c,9000)==-1);
    assert(!f.hci.configuration_deadline && f.accesses==before);config_service_current=1;
    /* Capability removal is a local refusal before configuration/UART mutation. */
    f.hci.identity.supported_commands[7]&=(uint8_t)~4u;
    assert(bt_hci_configure_incoming(&f.hci,&c,9000)==-1 && f.accesses==before);
    config_identify(&f);config_start(&f);assert(bt_acl_init(&a,&f.hci)==-1);
    config_send(&f,0);before=f.accesses;f.now=f.hci.command_deadline;
    assert(bt_hci_step(&f.hci,64,10000)==-1 && f.accesses==before && !f.hci.configured);
    config_identify(&f);c=configuration(&f);assert(bt_hci_configure_incoming(&f.hci,&c,2)==1);
    f.now=2;before=f.accesses;assert(bt_hci_step(&f.hci,64,10000)==-1 && f.accesses==before);
    config_identify(&f);config_start(&f);config_send(&f,0);config_service_current=0;
    before=f.accesses;assert(bt_hci_step(&f.hci,64,10000)==-1 && f.accesses==before);
    config_identify(&f);config_start(&f);config_through(&f,8);config_service_current=0;
    before=f.accesses;assert(bt_acl_init(&a,&f.hci)==-1 && !a.hci && !f.hci.acl_owner && f.accesses==before);
    config_identify(&f);assert(bt_acl_init(&a,&f.hci)==1);
    assert(bt_acl_incoming(&a,2000)==-1 && !a.incoming_timeout_us);
    c=configuration(&f);assert(bt_hci_configure_incoming(&f.hci,&c,9000)==-1);
}
static void malformed_and_ordered(void)
{
    struct fixture f;
    /* Every failed configuration command is retained, never publishes ready. */
    for(unsigned failed=0;failed<8;++failed) {
        config_identify(&f);config_start(&f);config_through(&f,failed);config_send(&f,failed);
        config_reply(&f,failed,0x0c);assert(bt_hci_step(&f.hci,64,10000)==-1);
        assert(!f.hci.configured && f.hci.configuration_stage==failed && bt_hci_next(&f.hci));
    }
    const unsigned reads[]={3,5,7};
    for(unsigned i=0;i<3;++i) {
        unsigned stage=reads[i];config_identify(&f);config_start(&f);config_through(&f,stage);config_send(&f,stage);
        config_reply(&f,stage,0);f.rx[f.rn-1]^=1;
        assert(bt_hci_step(&f.hci,64,10000)==-1 && !f.hci.configured && bt_hci_next(&f.hci));
    }
    config_identify(&f);config_start(&f);config_send(&f,0);
    cs(&f,configuration_ops[0],1,0);assert(bt_hci_step(&f.hci,64,10000)==-1 && !f.hci.configured);
}
static void stale_and_credit(void)
{
    struct fixture f;config_identify(&f);config_start(&f);
    /* Pre-acquired matching Complete cannot satisfy the later command. */
    config_reply(&f,0,0);step(&f,64);
    assert(f.hci.command==BT_HCI_SENDING && f.hci.configuration_stage==0);
    const struct bt_h4_record *old=bt_hci_next(&f.hci);assert(old && !old->tag);
    config_send(&f,0);
    cc(&f,configuration_ops[1],7,1,0);step(&f,64);
    assert(!f.hci.credits && f.hci.configuration_stage==0);
    config_reply(&f,0,0);step(&f,64);assert(f.hci.configuration_stage==1);
    assert(bt_hci_release(&f.hci,old->serial)==1);
    const struct bt_h4_record *wrong=bt_hci_next(&f.hci);assert(wrong && bt_hci_release(&f.hci,wrong->serial)==1);
    config_send(&f,1);uint8_t ack[]={0,5,12,0};event(&f,HCI_EV_CMD_COMPLETE,ack,sizeof(ack));step(&f,64);
    assert(f.hci.configuration_stage==2 && f.hci.command==BT_HCI_IDLE && !f.hci.credits);
    size_t before=f.tn;step(&f,64);assert(f.tn==before && f.hci.command==BT_HCI_IDLE);
    const uint8_t credit[]={1,0,0};event(&f,HCI_EV_CMD_COMPLETE,credit,sizeof(credit));step(&f,64);
    assert(f.hci.command==BT_HCI_SENDING && f.hci.configuration_stage==2);
    /* Wrong return length faults before reading the missing timeout bytes. */
    config_send(&f,2);config_reply(&f,2,0);step(&f,64);config_send(&f,3);
    cc(&f,configuration_ops[3],1,1,0);assert(bt_hci_step(&f.hci,64,10000)==-1);
}
int main(void)
{
    configured_handoff(0);configured_handoff(1);unreserved_handoff();
    refusal_and_deadlines();malformed_and_ordered();stale_and_credit();
    puts("Actual UART/H4/HCI configuration, readbacks, retained early RX and partial-frame ACL handoff, timeout/authority refusal, unique owner, incoming Accept/Reject and native consumer ABI passed.");
    return 0;
}
