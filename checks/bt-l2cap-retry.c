/* Reuse the actual UART/H4/HCI/ACL fixture. The earlier model main is compiled
 * for API compatibility but is not executed by this changed-behavior check. */
#define main bt_l2cap_previous_model
#include "bt-l2cap.c"
#undef main

static unsigned begin_request(struct simulation *s,uint64_t deadline,uint64_t *identity)
{
    assert(bt_l2cap_open(&s->q,s->link,1,deadline,identity)==1);
    assert(s->q.channels[0].timer==BT_L2CAP_TIMER_NONE);
    unsigned out=output(s);
    const struct l2cap_cmd_hdr *r=command_out(s,out,L2CAP_CONN_REQ);
    assert(s->q.channels[0].request_sent && s->q.channels[0].timer==BT_L2CAP_RTX);
    return r->ident;
}
static void retirement_after_timeout(void)
{
    struct simulation s;setup(&s);s.automatic_credits=0;uint64_t identity;
    unsigned id=begin_request(&s,UINT64_MAX,&identity);assert(id==1 && s.a.credits==1);
    uint64_t request=s.q.channels[0].request_serial;
    assert(s.q.channels[0].timer_deadline>s.f.now);
    size_t tx=s.f.tn;s.f.now=s.q.channels[0].timer_deadline;poll(&s);
    assert(s.q.state==BT_L2CAP_ACTIVE && s.q.channels[0].state==BT_L2CAP_QUARANTINED &&
        s.q.channels[0].completion==BT_L2CAP_RTX_EXPIRED && s.f.tn==tx && s.a.credits==1);
    assert(s.q.tx[0].serial==request && s.q.tx[0].leases==1 && bt_l2cap_release_channel(&s.q,identity)==-1);
    const uint8_t credit[]={1,0x23,0,1,0};event(&s.f,HCI_EV_NUM_COMP_PKTS,credit,sizeof(credit));poll(&s);
    assert(s.a.credits==2 && s.q.tx[0].state==BT_L2CAP_FREE);
    struct l2cap_conn_rsp late={0x60,s.q.channels[0].local_cid,0,0};signal_in(&s,L2CAP_CONN_RSP,id,&late,8);poll(&s);
    assert(s.q.channels[0].state==BT_L2CAP_QUARANTINED && bt_l2cap_release_channel(&s.q,identity)==-1);
    setup(&s);assert(bt_l2cap_open(&s.q,s.link,1,s.f.now+1,&identity)==1);tx=s.f.tn;poll(&s);
    assert(s.q.channels[0].completion==BT_L2CAP_CALLER_EXPIRED && s.q.rotation_broken[0] &&
        s.f.tn==tx && s.q.tx[0].state==BT_L2CAP_FREE && s.a.credits==2);
}
static void pending_and_ertx(void)
{
    struct simulation s;setup(&s);uint64_t identity;unsigned id=begin_request(&s,UINT64_MAX,&identity);
    /* Non-success endpoint fields may be invalid and must be ignored. */
    struct l2cap_conn_rsp pending={0,0,1,1};signal_in(&s,L2CAP_CONN_RSP,id,&pending,8);poll(&s);
    assert(s.q.channels[0].timer==BT_L2CAP_ERTX);
    uint64_t bound=s.q.channels[0].timer_deadline;
    s.f.now+=s.q.timeout_us+1;signal_in(&s,L2CAP_CONN_RSP,id,&pending,8);poll(&s);
    assert(s.q.channels[0].timer_deadline==bound && s.q.channels[0].state==BT_L2CAP_CONNECTING);
    s.f.now=bound;poll(&s);
    assert(s.q.channels[0].completion==BT_L2CAP_ERTX_EXPIRED && s.q.state==BT_L2CAP_ACTIVE);
    setup(&s);id=begin_request(&s,UINT64_MAX,&identity);
    signal_in(&s,L2CAP_CONN_RSP,id,&pending,8);poll(&s);
    struct l2cap_conn_rsp success={0x60,s.q.channels[0].local_cid,0,0};signal_in(&s,L2CAP_CONN_RSP,id,&success,8);
    unsigned out=output(&s);command_out(&s,out,L2CAP_CONF_REQ);
    assert(s.q.channels[0].state==BT_L2CAP_CONFIGURING && s.q.channels[0].timer==BT_L2CAP_RTX);
    setup(&s);uint64_t caller=s.f.now+1000;id=begin_request(&s,caller,&identity);
    signal_in(&s,L2CAP_CONN_RSP,id,&pending,8);poll(&s);s.f.now=caller;poll(&s);
    assert(s.q.channels[0].completion==BT_L2CAP_CALLER_EXPIRED && s.q.state==BT_L2CAP_ACTIVE);
}
static void close_cancel_and_reuse(void)
{
    struct simulation s;setup(&s);uint64_t old=open_channel(&s);uint16_t cid=s.q.channels[0].local_cid;
    const uint8_t payload[]={1,2};incoming_pdu(&s,cid,payload,sizeof(payload),0);poll(&s);
    const struct bt_l2cap_pdu *rx=bt_l2cap_receive(&s.q);assert(rx && rx->channel==old);uint64_t receive=rx->serial;
    assert(bt_l2cap_close(&s.q,old,UINT64_MAX)==1);
    struct l2cap_disconn_req pair={cid,0x60};signal_in(&s,L2CAP_DISCONN_REQ,41,&pair,4);poll(&s);
    assert(s.q.channels[0].state==BT_L2CAP_CLOSED && s.q.rotation_broken[0]);
    unsigned out=output(&s);command_out(&s,out,L2CAP_DISCONN_RSP);
    assert(bt_l2cap_release_channel(&s.q,old)==0);
    assert(bt_l2cap_release_receive(&s.q,receive)==1 && bt_l2cap_release_channel(&s.q,old)==1);
    uint64_t next;assert(bt_l2cap_open(&s.q,s.link,1,UINT64_MAX,&next)==1);
    assert(next!=old && s.q.channels[0].local_cid!=cid && bt_l2cap_close(&s.q,old,UINT64_MAX)==-1);
    out=output(&s);const struct l2cap_cmd_hdr *req=command_out(&s,out,L2CAP_CONN_REQ);
    struct l2cap_conn_rsp stale={0x60,cid,0,0};signal_in(&s,L2CAP_CONN_RSP,req->ident,&stale,8);poll(&s);
    assert(s.q.channels[0].state==BT_L2CAP_CONNECTING);
    /* Exercise the actual 8-bit rotation: canceled ID3 must not count as sent. */
    for(unsigned id=req->ident;id<=255;++id) {
        struct l2cap_conn_rsp refused={0,0,2,0};signal_in(&s,L2CAP_CONN_RSP,id,&refused,8);poll(&s);
        assert(s.q.channels[0].completion==BT_L2CAP_CONFIRMED && bt_l2cap_release_channel(&s.q,next)==1);
        s.outputs=0;
        int result=bt_l2cap_open(&s.q,s.link,1,UINT64_MAX,&next);
        if(id==255)assert(result==BT_L2CAP_EXHAUSTED && s.q.state==BT_L2CAP_ACTIVE);
        else {assert(result==1);out=output(&s);assert(command_out(&s,out,L2CAP_CONN_REQ)->ident==id+1);}
    }
}
static void local_rotation(void)
{
    struct simulation s;setup(&s);uint64_t identity;
    for(unsigned id=1;id<=255;++id) {
        unsigned actual=begin_request(&s,UINT64_MAX,&identity);assert(actual==id);
        struct l2cap_conn_rsp refused={0,0,2,0};signal_in(&s,L2CAP_CONN_RSP,id,&refused,8);poll(&s);
        assert(s.q.channels[0].completion==BT_L2CAP_CONFIRMED && bt_l2cap_release_channel(&s.q,identity)==1);
        s.outputs=0;
    }
    assert(begin_request(&s,UINT64_MAX,&identity)==1 && s.q.channels[0].request_reused);
    struct l2cap_conn_rsp stale={0x60,0x40,0,0};signal_in(&s,L2CAP_CONN_RSP,1,&stale,8);poll(&s);
    assert(s.q.channels[0].state==BT_L2CAP_CONNECTING);
    struct l2cap_conn_rsp ambiguous={0,0,1,0};signal_in(&s,L2CAP_CONN_RSP,1,&ambiguous,8);
    assert(bt_l2cap_step(&s.q,64,UINT64_MAX)==-1 && s.q.error==BT_L2CAP_OWNERSHIP &&
        bt_l2cap_release_channel(&s.q,identity)==-1);
}
static void peer_rotation(void)
{
    struct simulation s;setup(&s);
    for(unsigned id=1;id<=255;++id) {
        uint8_t byte=(uint8_t)id;signal_in(&s,L2CAP_ECHO_REQ,id,&byte,1);unsigned out=output(&s);
        const struct l2cap_cmd_hdr *reply=command_out(&s,out,9);
        assert(reply->ident==id && reply->len==1 && ((const uint8_t *)reply)[4]==byte);
        s.outputs=0;
    }
    uint8_t replacement=77;signal_in(&s,L2CAP_ECHO_REQ,1,&replacement,1);unsigned out=output(&s);
    assert(((const uint8_t *)command_out(&s,out,9))[4]==replacement && s.q.peer_sequence[0]==256);
    signal_in(&s,L2CAP_ECHO_REQ,1,&replacement,1);out=output(&s);
    assert(((const uint8_t *)command_out(&s,out,9))[4]==replacement && s.q.peer_sequence[0]==256);
    replacement=78;signal_in(&s,L2CAP_ECHO_REQ,1,&replacement,1);
    assert(bt_l2cap_step(&s.q,64,UINT64_MAX)==-1 && s.q.error==BT_L2CAP_PROTOCOL);
}
static void final_response_under_backpressure(void)
{
    struct simulation s;setup(&s);uint64_t first=open_channel(&s),second;
    assert(bt_l2cap_open(&s.q,s.link,1,UINT64_MAX,&second)==1);
    unsigned out=output(&s);const struct l2cap_cmd_hdr *req=command_out(&s,out,L2CAP_CONN_REQ);
    uint16_t cid=s.q.channels[1].local_cid;struct l2cap_conn_rsp success={0x62,cid,0,0};
    signal_in(&s,L2CAP_CONN_RSP,req->ident,&success,8);out=output(&s);
    unsigned config=command_out(&s,out,L2CAP_CONF_REQ)->ident;
    for(unsigned i=0;i<BT_L2CAP_PDUS;++i) {
        uint8_t payload=(uint8_t)i;uint64_t serial;
        assert(bt_l2cap_send(&s.q,first,&payload,1,UINT64_MAX,&serial)==1);output(&s);
    }
    const uint8_t refused[]={ (uint8_t)cid,(uint8_t)(cid>>8),0,0,2,0};
    signal_in(&s,L2CAP_CONF_RSP,config,refused,6);poll(&s);
    assert(s.q.channels[1].timer==BT_L2CAP_TIMER_NONE && s.q.channels[1].state==BT_L2CAP_CONFIGURING);
    const struct bt_l2cap_pdu *done=bt_l2cap_result(&s.q);assert(done);
    assert(bt_l2cap_release_result(&s.q,done->serial)==1);
    out=output(&s);command_out(&s,out,L2CAP_DISCONN_REQ);
    assert(s.q.channels[1].state==BT_L2CAP_DISCONNECTING && s.q.channels[1].timer==BT_L2CAP_RTX);
}
int main(void)
{
    retirement_after_timeout();pending_and_ertx();close_cancel_and_reuse();
    local_rotation();peer_rotation();final_response_under_backpressure();
    puts("L2CAP changed behavior: RTX/ERTX, caller expiry, retained credits, confirmed-close cancellation, opaque/CID reuse, full identifier rotations and capacity-delayed response passed.");
    return 0;
}
