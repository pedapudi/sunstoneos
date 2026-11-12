/* Reuse the pinned Core/HFP wire model and actual UART/HCI/ACL/SCO workers.
 * The fixture's security callback supplies an admitted, unchanged service. */
#include "headset-sco-fixture.h"
static struct audio_fixture scan;
static uint64_t scan_partial_serial;
static unsigned scan_partial_offset;
/* Production consumer-step code; generated field bindings select this fixture's
 * real owner graph without replacing any protocol operation. */
#include "headset-scan-step.h"
static void owned_scan_input(void)
{
    opened(&scan);struct simulation *s=&scan.g.rf.d.lower;struct fixture *h=&s->f;
    for(unsigned i=0;i<9;++i){uint8_t byte=(uint8_t)i;sco_input(&scan,0,0,&byte,1);assert(audio_poll(&scan)==1);}
    unsigned held=0;for(unsigned i=0;i<BT_H4_SLOTS;++i)held+=scan.q.held_rx[i]!=0;assert(held==1);
    uint8_t frame[16];const uint8_t newline='\r';
    unsigned n=reference_frame(frame,14,RFCOMM_UIH,1,&newline,1,-1);
    incoming_pdu(s,0x41,frame,n,0);
    for(unsigned i=0;i<3;++i){++h->now;h->room=1024;assert(bt_acl_step(&s->a,64,90000)==1);}
    assert(bt_acl_receive(&s->a));
    assert(bt_acl_close_admission(&s->a,90000)==1&&bt_sco_close_admission(&scan.q,90000)==1);
    assert(bt_hci_scan_disable(&h->hci,90000)==1);
    unsigned replied=0;
    for(unsigned i=0;i<50&&!bt_hci_scan_disabled(&h->hci);++i){
        ++h->now;h->room=1024;assert(stop_consumers_step(90000)>=0);
        assert(scan.q.links[0].phase==BT_SCO_OPEN&&!scan.q.stop_close);
        if(!replied&&h->hci.command==BT_HCI_WAITING&&h->hci.opcode==0x0c1a){
            cc(h,0x0c1a,1,1,0);replied=1;}
    }
    assert(replied&&bt_hci_scan_disabled(&h->hci)==1&&!bt_acl_receive(&s->a));
    for(unsigned i=0;i<BT_H4_SLOTS;++i)assert(!scan.q.held_rx[i]);
    puts("PASS production consumer round with owned ACL and full copied SCO receive pool");
}
static void stopped_disconnect(struct audio_fixture *f)
{
    command(f,0x0406);status(f,0x0406,0);
    assert(bt_sco_quiescent(&f->q)==0);
    const uint8_t done[]={0,0,0,0x13};event(&f->g.rf.d.lower.f,5,done,4);
    assert(audio_poll(f)==1);
}
static void owned_voice_input(void)
{
    memset(&scan,0,sizeof(scan));voice_establish(&scan.g,BT_HFP_AG_HF_VOICE);scan.authority=1;
    struct bt_sco_binding b={27,1000,2000,audio_current,audio_security,&scan};
    assert(bt_sco_init(&scan.q,&scan.g.rf.d.lower.a,&b)==1);
    assert(bt_sco_open(&scan.q,&scan.g.ag,0,90000,&scan.link)==1);
    remote(&scan,1);enhanced(&scan,0,1);complete(&scan,0,0,2);
    struct simulation *s=&scan.g.rf.d.lower;struct fixture *h=&s->f;s->wire_cursor=h->tn;
    assert(bt_hfp_ag_voice(&scan.g.ag,1)==1);response_bytes(&scan.g,"\r\n+BVRA: 1\r\n");
    assert(scan.g.ag.recognition&&bt_acl_close_admission(&s->a,90000)==1);
    assert(bt_sco_close_admission(&scan.q,90000)==1&&bt_hfp_ag_voice(&scan.g.ag,0)==1);
    for(unsigned i=0;i<8;++i){++h->now;h->room=1024;assert(bt_hfp_ag_step(&scan.g.ag,64,128,90000)>=0);}
    assert(scan.g.ag.transmit&&scan.g.ag.response_length);
    for(unsigned i=0;i<11;++i){uint8_t byte=(uint8_t)i;sco_input(&scan,0,0,&byte,1);assert(audio_poll(&scan)==1);}
    unsigned held=0;for(unsigned i=0;i<BT_H4_SLOTS;++i)held+=scan.q.held_rx[i]!=0;assert(held==3);
    wire(s);
    for(unsigned i=0;i<80&&(scan.g.ag.response_length||scan.g.ag.transmit);++i){
        ++h->now;h->room=1024;assert(stop_consumers_step(90000)>=0);
        assert(scan.q.links[0].phase==BT_SCO_OPEN&&!scan.q.stop_close);wire(s);
    }
    assert(!scan.g.ag.recognition&&!scan.g.ag.response_length&&!scan.g.ag.transmit);
    puts("PASS recognition-off completion with copied SCO pool and every H4 receive slot held");
}
static void concurrent_discovery_input(void)
{
    opened(&scan);struct discovery *d=&scan.g.rf.d;struct simulation *s=&d->lower;
    struct fixture *h=&s->f;s->wire_cursor=h->tn;d->next_output=s->outputs;
    unsigned cid=s->q.next_cid;
    assert(bt_sdp_begin(&d->client,&s->q,s->link,HANDSFREE_SVCLASS_ID,90000)==1);
    const struct l2cap_cmd_hdr *request=command_out(s,doutput(d),L2CAP_CONN_REQ);
    struct l2cap_conn_rsp success={0x62,(uint16_t)cid,0,0};signal_in(s,L2CAP_CONN_RSP,request->ident,&success,8);
    request=command_out(s,doutput(d),L2CAP_CONF_REQ);
    uint8_t accepted[]={cid,cid>>8,0,0,0,0};signal_in(s,L2CAP_CONF_RSP,request->ident,accepted,6);dpoll(d);
    uint8_t peer[]={cid,cid>>8,0,0,1,2,0xa0,2};signal_in(s,L2CAP_CONF_REQ,44,peer,8);
    command_out(s,doutput(d),L2CAP_CONF_RSP);
    for(unsigned i=0;i<8;++i)dpoll(d);
    assert(d->client.phase==BT_SDP_RUNNING&&d->client.sent);
    /* Eight accepted server requests occupy all copied L2CAP input slots.
     * The client's later close response needs one of those slots to parse. */
    for(unsigned i=0;i<BT_L2CAP_PDUS;++i){
        uint8_t query[]={4,0,(uint8_t)(i+2),0,14,0,1,0,1,0xff,0xff,0x35,5,0x0a,0,0,0xff,0xff,0};
        incoming_pdu(s,0x40,query,sizeof(query),0);dpoll(d);
    }
    for(unsigned i=0;i<BT_L2CAP_PDUS;++i)assert(s->q.rx[i].state==BT_L2CAP_RECEIVED);
    assert(bt_acl_close_admission(&s->a,90000)==1&&bt_sco_close_admission(&scan.q,90000)==1);
    assert(bt_sdp_close(&d->client,90000)==1);unsigned cursor=s->outputs,replied=0;
    for(unsigned i=0;i<200&&d->client.phase!=BT_SDP_CLOSED;++i){
        ++h->now;h->room=1024;assert(stop_consumers_step(90000)>=0);wire(s);
        while(cursor<s->outputs){const uint8_t *p=s->output[cursor++];
            if(p[2]==1&&p[3]==0&&p[4]==L2CAP_DISCONN_REQ){
                signal_in(s,L2CAP_DISCONN_RSP,p[5],p+8,4);replied=1;}}
    }
    assert(replied&&d->client.phase==BT_SDP_CLOSED);
    puts("PASS concurrent SDP server input drains while the client closes");
}
static void refusal_cadence(void)
{
    opened(&scan);struct fixture *h=&scan.g.rf.d.lower.f;
    assert(bt_acl_close_admission(&scan.g.rf.d.lower.a,90000)==1&&bt_sco_close_admission(&scan.q,90000)==1);
    const uint8_t request[]={9,2,3,4,5,6,4,4,0x24,2};event(h,4,request,10);
    /* A request arrives before the next owner pass. After one millisecond,
     * the fixed consumer round must queue and transmit its bounded refusal. */
    h->now+=1000;h->room=1024;assert(stop_consumers_step(90000)>=0);
    assert(scan.q.refuse==2&&h->hci.opcode==0x042a&&h->hci.command==BT_HCI_WAITING);
    assert(scan.q.refuse_deadline-h->now==2000&&!scan.q.stop_close);
    puts("PASS synchronous refusal transmission on the next 1 ms owner pass");
}
static int acl_stop_progress(void)
{
    struct simulation *s=&scan.g.rf.d.lower;++s->f.now;s->f.room=1024;
    return stop_acl_step(90000);
}
static void late_refusal_after_acl_stop(void)
{
    opened(&scan);assert(bt_sco_stop(&scan.q,90000)==1);stopped_disconnect(&scan);
    struct bt_acl *a=&scan.g.rf.d.lower.a;struct fixture *h=&scan.g.rf.d.lower.f;
    assert(bt_sco_quiescent(&scan.q)==1&&bt_acl_stop(a,90000)==1);
    for(unsigned i=0;i<8&&h->hci.command!=BT_HCI_WAITING;++i)assert(acl_stop_progress()==1);
    assert(h->hci.opcode==0x0406&&h->hci.command==BT_HCI_WAITING);cs(h,0x0406,1,0);assert(acl_stop_progress()==1);
    assert(a->links[0].state==BT_ACL_DISCONNECTING);
    const uint8_t payload[]={1,2,3};incoming_pdu(&scan.g.rf.d.lower,0x41,payload,sizeof(payload),0);
    assert(acl_stop_progress()==1&&!bt_acl_receive(a));
    const uint8_t request[]={9,2,3,4,5,6,4,4,0x24,2};event(h,4,request,10);assert(acl_stop_progress()==1&&scan.q.refuse);
    const uint8_t done[]={0,0x23,0,0x13};event(h,5,done,4);assert(acl_stop_progress()==1);
    assert(a->links[0].state==BT_ACL_CLOSED&&bt_sco_quiescent(&scan.q)==0);
    for(unsigned i=0;i<8&&h->hci.command!=BT_HCI_WAITING;++i)assert(acl_stop_progress()==1);
    assert(h->hci.opcode==0x042a&&h->hci.command==BT_HCI_WAITING);cs(h,0x042a,1,0);assert(acl_stop_progress()==1);
    uint8_t refused[]={0x0d,0,0,9,2,3,4,5,6,2,0,0,0,0,0,0,0};event(h,0x2c,refused,sizeof(refused));
    assert(acl_stop_progress()==1&&bt_sco_quiescent(&scan.q)==1&&bt_acl_quiescent(a)==1);
    const uint8_t unknown[]={2,0x24,0x20,1,0,0};bytes(h,unknown,sizeof(unknown));
    assert(acl_stop_progress()==-1);
    puts("PASS known late ACL data, matched SCO refusal and unknown-handle rejection during ACL retirement");
}
int main(int argc,char **argv)
{
    if(argc==2&&!strcmp(argv[1],"--voice-only")){owned_voice_input();return 0;}
    if(argc==2&&!strcmp(argv[1],"--discovery-only")){concurrent_discovery_input();return 0;}
    if(argc==2&&!strcmp(argv[1],"--cadence-only")){refusal_cadence();return 0;}
    assert(argc==1);
    owned_scan_input();
    owned_voice_input();
    concurrent_discovery_input();
    refusal_cadence();
    late_refusal_after_acl_stop();
    struct audio_fixture f;uint64_t id=0;
    audio_start(&f,0,0);size_t wire=f.g.rf.d.lower.f.tn;
    assert(bt_sco_stop(&f.q,90000)==1&&bt_sco_quiescent(&f.q)==1);
    assert(audio_poll(&f)==1&&f.g.rf.d.lower.f.tn==wire);
    assert(bt_sco_open(&f.q,&f.g.ag,0,90000,&id)==-1);
    uint8_t pcm[2]={0};assert(bt_sco_send(&f.q,f.link,pcm,2,90000,&id)==-1);
    audio_start(&f,0,0);command(&f,0x041b);
    uint64_t original=f.g.rf.d.lower.f.hci.command_deadline;
    assert(bt_sco_stop(&f.q,90000)==1&&bt_sco_quiescent(&f.q)==0);
    assert(f.g.rf.d.lower.f.hci.command_deadline==original);
    status(&f,0x041b,0);
    uint8_t features[]={0,0x23,0,0,0x28,1,0x80,0,0x20,0,0};
    event(&f.g.rf.d.lower.f,0x0b,features,sizeof(features));
    wire=f.g.rf.d.lower.f.tn;assert(audio_poll(&f)==1);
    assert(bt_sco_quiescent(&f.q)==1&&f.g.rf.d.lower.f.tn==wire);
    audio_start(&f,0,0);remote(&f,1);enhanced(&f,0,1);
    assert(bt_sco_stop(&f.q,90000)==1&&f.q.links[0].phase==BT_SCO_SETUP_WAIT);
    uint8_t complete_event[]={0,0,0,1,2,3,4,5,6,2,6,2,60,0,60,0,2};
    event(&f.g.rf.d.lower.f,0x2c,complete_event,sizeof(complete_event));assert(audio_poll(&f)==1);
    stopped_disconnect(&f);assert(bt_sco_quiescent(&f.q)==1);
    opened(&f);uint64_t sent=transmit(&f);assert(bt_sco_stop(&f.q,90000)==1);
    stopped_disconnect(&f);assert(bt_sco_quiescent(&f.q)==0);
    assert(bt_sco_release_result(&f.q,sent)==1&&bt_sco_quiescent(&f.q)==1);
    opened(&f);assert(bt_sco_stop(&f.q,90000)==1);command(&f,0x0406);
    cs(&f.g.rf.d.lower.f,0x0406,1,0x0c);assert(audio_poll(&f)==-1);
    wire=f.g.rf.d.lower.f.tn;assert(bt_sco_stop(&f.q,95000)==-1);
    assert(bt_sco_quiescent(&f.q)==-1&&f.g.rf.d.lower.f.tn==wire);
    audio_start(&f,0,0);command(&f,0x041b);
    original=f.g.rf.d.lower.f.hci.command_deadline;assert(bt_sco_stop(&f.q,90000)==1);
    f.g.rf.d.lower.f.now=original;assert(audio_poll(&f)==-1&&bt_sco_quiescent(&f.q)==-1);
    opened(&f);struct fixture *h=&f.g.rf.d.lower.f;uint64_t packet=0;
    assert(bt_sco_send(&f.q,f.link,pcm,2,100000,&packet)==1);
    h->room=1;++h->now;assert(bt_sco_step(&f.q,1,100000)==1);
    ++h->now;assert(bt_sco_step(&f.q,1,100000)==1&&f.q.active_tx<BT_SCO_PACKETS);
    assert(bt_sco_stop(&f.q,h->now+5)==1&&bt_sco_quiescent(&f.q)==0);
    h->room=0;h->now+=5;assert(bt_sco_step(&f.q,1,100000)==-1&&bt_sco_quiescent(&f.q)==-1);
    assert(f.q.tx[f.q.active_tx].serial==packet&&f.q.tx[f.q.active_tx].state==BT_SCO_PARTIAL);
    audio_start(&f,1,0);remote(&f,1);assert(f.q.links[0].phase==BT_SCO_LISTENING);
    wire=f.g.rf.d.lower.f.tn;assert(bt_sco_stop(&f.q,90000)==1&&bt_sco_quiescent(&f.q)==1);
    assert(audio_poll(&f)==1&&f.g.rf.d.lower.f.tn==wire);
    audio_start(&f,1,0);command(&f,0x041b);status(&f,0x041b,0);
    const uint8_t request[]={1,2,3,4,5,6,4,4,0x24,2};event(&f.g.rf.d.lower.f,4,request,10);
    assert(audio_poll(&f)==1&&f.q.links[0].requested&&f.q.links[0].phase==BT_SCO_FEATURE_WAIT);
    assert(bt_sco_stop(&f.q,90000)==1);
    event(&f.g.rf.d.lower.f,0x0b,features,sizeof(features));assert(audio_poll(&f)==1);command(&f,0x042a);
    cs(&f.g.rf.d.lower.f,0x042a,1,0x0c);assert(audio_poll(&f)==-1&&bt_sco_quiescent(&f.q)==-1);
    puts("PASS nine SCO return cases including partial-frame retention, listener closure and reject refusal");
    return 0;
}
