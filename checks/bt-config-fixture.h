/* SPDX-License-Identifier: Apache-2.0 */
/* Shared synthetic controller replies over the actual UART/H4/HCI workers.
 * No configured flags or identity capability fields are fabricated in memory. */
#ifndef SUNSTONE_BT_CONFIG_FIXTURE_H
#define SUNSTONE_BT_CONFIG_FIXTURE_H
static unsigned config_service_current=1;
static int configuration_current(void *p,const struct bt_h4_binding *binding,
                                 uint64_t generation,uint64_t latency)
{
    struct fixture *f=p;
    return config_service_current && f->current && binding->uart==&f->uart &&
        binding->firmware_generation==4 && binding->uart_generation==9 &&
        generation==17 && latency==1000;
}
static struct bt_hci_incoming_config configuration(struct fixture *f)
{
    return (struct bt_hci_incoming_config){.accept_timeout_slots=0x1fa0,
        .page_interval_slots=0x0800,.page_window_slots=0x0012,
        .service_generation=17,.receive_latency_us=1000,
        .current=configuration_current,.context=f};
}
static void config_identify(struct fixture *f)
{
    init(f);config_service_current=1;
    const unsigned ops[]={HCI_OP_RESET,HCI_OP_READ_LOCAL_VERSION,HCI_OP_READ_LOCAL_COMMANDS,
        HCI_OP_READ_LOCAL_FEATURES,HCI_OP_READ_BUFFER_SIZE,HCI_OP_READ_BD_ADDR};
    const unsigned sizes[]={1,9,65,9,8,7};
    for(unsigned i=0;i<6;++i) {
        send(f,ops[i]);
        if(i==2) {
            uint8_t reply[68]={1,2,16,0};
            reply[4]=0x30;reply[5]=3;reply[9]=0x40;reply[10]=1;
            reply[11]=0xcc;reply[12]=3;reply[67]=0x70;
            event(f,HCI_EV_CMD_COMPLETE,reply,sizeof(reply));
        } else cc(f,ops[i],1,sizes[i],0);
        for(unsigned j=0;j<4 && f->hci.stage==i;++j)step(f,64);
        assert(f->hci.stage==i+1);
    }
    assert(f->hci.state==BT_HCI_IDENTIFIED && f->hci.command_tag==6);
}
static const unsigned configuration_ops[]={0x0c01,0x0c05,0x0c16,0x0c15,0x0c1c,0x0c1b,0x0c1a,0x0c19};
static const unsigned configuration_lengths[]={8,1,2,0,4,0,1,0};
static void config_send(struct fixture *f,unsigned stage)
{
    if(f->hci.command==BT_HCI_IDLE)step(f,64);
    assert(stage<8 && f->hci.configuration_stage==stage && f->hci.command==BT_HCI_SENDING);
    size_t before=f->tn;step(f,64);
    assert(f->hci.command==BT_HCI_WAITING && f->tn==before+4+configuration_lengths[stage]);
    uint8_t expected[12]={1,configuration_ops[stage],configuration_ops[stage]>>8,configuration_lengths[stage]};
    if(stage==0) {
        /* Independent event-number list from Core5.4 §7.3.1, including
         * Classic security/synchronous events, excluding LE and reserved bits. */
        const unsigned events[]={1,2,3,4,5,6,7,8,9,10,11,12,13,16,17,18,20,21,22,23,24,
            25,26,27,28,29,30,32,33,34,35,44,45,46,47,48,49,50,51,52,53,54,56,57,59,60,61};
        for(unsigned j=0;j<sizeof(events)/sizeof(events[0]);++j)
            expected[4+(events[j]-1)/8]|=(uint8_t)(1u<<((events[j]-1)%8));
    } else if(stage==2) {expected[4]=0xa0;expected[5]=0x1f;}
    else if(stage==4) {expected[4]=0;expected[5]=8;expected[6]=0x12;}
    else if(stage==6) expected[4]=2;
    assert(!memcmp(expected,f->tx+before,4+configuration_lengths[stage]));
}
static void config_reply(struct fixture *f,unsigned stage,unsigned status)
{
    const unsigned sizes[]={1,1,1,3,1,5,1,2};
    uint8_t p[8]={1,configuration_ops[stage],configuration_ops[stage]>>8,(uint8_t)status};
    if(stage==3) {p[4]=0xa0;p[5]=0x1f;}
    else if(stage==5) {p[4]=0;p[5]=8;p[6]=0x12;}
    else if(stage==7) p[4]=2;
    event(f,HCI_EV_CMD_COMPLETE,p,3+sizes[stage]);
}
static void config_start(struct fixture *f)
{
    struct bt_hci_incoming_config c=configuration(f);
    assert(bt_hci_configure_incoming(&f->hci,&c,9000)==1);
    c.accept_timeout_slots=1; /* Runtime keeps its own immutable copy. */
    assert(f->hci.incoming_config.accept_timeout_slots==0x1fa0);
}
static void config_through(struct fixture *f,unsigned count)
{
    for(unsigned i=0;i<count;++i) {
        config_send(f,i);config_reply(f,i,0);step(f,64);
        assert(f->hci.configuration_stage==i+1);
    }
}
static void config_ready(struct fixture *f,struct bt_acl *a)
{
    config_identify(f);config_start(f);config_through(f,8);
    memset(a,0,sizeof(*a));assert(bt_acl_init(a,&f->hci)==1);
}
#endif
