#define main bt_l2cap_previous_model
#include "bt-l2cap.c"
#undef main
#include "bt_sdp.h"
#include "sdp-ref.h"
struct discovery {struct simulation lower;struct bt_sdp client;unsigned next_output;};
static void dpoll(struct discovery *d)
{
    d->lower.f.room=1024;++d->lower.f.now;
    int result=bt_sdp_step(&d->client,64,UINT64_MAX);
    if(result<0)fprintf(stderr,"SDP failure phase=%u status=%u lower=%u at=%llu\n",d->client.phase,d->client.status,
        d->lower.q.error,(unsigned long long)d->lower.f.now);
    assert(result>=0);wire(&d->lower);
}
static unsigned doutput(struct discovery *d)
{
    for(unsigned i=0;i<200 && d->next_output==d->lower.outputs;++i)dpoll(d);
    assert(d->next_output<d->lower.outputs);unsigned result=d->next_output++;
    for(unsigned i=0;i<4;++i)dpoll(d);return result;
}
static void dsetup(struct discovery *d,unsigned automatic,uint64_t interval)
{
    memset(d,0,sizeof(*d));setup(&d->lower);
    assert(bt_sdp_begin(&d->client,&d->lower.q,d->lower.link,HANDSFREE_SVCLASS_ID,d->lower.f.now+interval)==1);
    unsigned out=doutput(d);const struct l2cap_cmd_hdr *req=command_out(&d->lower,out,L2CAP_CONN_REQ);
    struct l2cap_conn_req reference={SDP_PSM,0x40};assert(!memcmp(req+1,&reference,4));
    struct l2cap_conn_rsp success={0x60,0x40,0,0};signal_in(&d->lower,L2CAP_CONN_RSP,req->ident,&success,8);
    out=doutput(d);req=command_out(&d->lower,out,L2CAP_CONF_REQ);
    const uint8_t accepted[]={0x40,0,0,0,0,0};signal_in(&d->lower,L2CAP_CONF_RSP,req->ident,accepted,6);dpoll(d);
    assert(d->client.phase==BT_SDP_OPENING && !d->client.tx_serial);
    const uint8_t peer[]={0x40,0,0,0,1,2,0xa0,2};signal_in(&d->lower,L2CAP_CONF_REQ,40,peer,8);
    d->lower.automatic_credits=(int)automatic;out=doutput(d);command_out(&d->lower,out,L2CAP_CONF_RSP);
}
static const uint8_t *srequest(struct discovery *d,unsigned transaction,const uint8_t *token,unsigned token_length)
{
    unsigned out=doutput(d);const uint8_t *p=d->lower.output[out];
    struct l2cap_hdr l;memcpy(&l,p,4);assert(l.cid==0x60 && l.len==20+token_length);
    uint8_t reference[36]={SDP_SVC_SEARCH_ATTR_REQ,0,0,0,0,SDP_SEQ8,3,SDP_UUID16,0x11,0x1e,
        2,0x88,SDP_SEQ8,5,SDP_UINT32,0,0,0xff,0xff,0};
    reference[1]=(uint8_t)(transaction>>8);reference[2]=(uint8_t)transaction;
    reference[4]=(uint8_t)(15+token_length);reference[19]=(uint8_t)token_length;
    if(token_length)memcpy(reference+20,token,token_length);
    assert(sizeof(sdp_pdu_hdr_t)==5 && !memcmp(p+4,reference,20+token_length));
    assert(d->client.sent && d->client.transaction==transaction);return p+4;
}
static void drx(struct discovery *d,const uint8_t *p,unsigned n)
{
    uint8_t packet[BT_L2CAP_MTU+4];struct l2cap_hdr h={(uint16_t)n,0x40};
    assert(n<=BT_L2CAP_MTU);memcpy(packet,&h,4);memcpy(packet+4,p,n);
    /* Actual HCI ACL fragmentation and receive reassembly, with bounded polls. */
    for(unsigned at=0;at<n+4;) {
        unsigned length=n+4-at;if(length>23)length=23;
        incoming_fragment(&d->lower,at ? 1 : 2,packet+at,length);dpoll(d);at+=length;
    }
    for(unsigned i=0;i<3;++i)dpoll(d);
}
static void srsp(struct discovery *d,unsigned tid,const uint8_t *data,unsigned n,const uint8_t *token,unsigned tn)
{
    uint8_t p[BT_L2CAP_MTU]={SDP_SVC_SEARCH_ATTR_RSP,0,0,0,0,0,0};
    assert(n+tn+8<=sizeof(p));p[1]=(uint8_t)(tid>>8);p[2]=(uint8_t)tid;
    p[3]=(uint8_t)((n+tn+3)>>8);p[4]=(uint8_t)(n+tn+3);p[5]=(uint8_t)(n>>8);p[6]=(uint8_t)n;
    memcpy(p+7,data,n);p[7+n]=(uint8_t)tn;if(tn)memcpy(p+8+n,token,tn);drx(d,p,n+tn+8);
}
struct encoded {uint8_t bytes[512];unsigned n;};
static void append(struct encoded *e,const uint8_t *p,unsigned n)
{assert(e->n+n<=sizeof(e->bytes));memcpy(e->bytes+e->n,p,n);e->n+=n;}
static void octet(struct encoded *e,unsigned v) {uint8_t p=(uint8_t)v;append(e,&p,1);}
static void uint16_element(struct encoded *e,unsigned n)
{const uint8_t p[]={SDP_UINT16,(uint8_t)(n>>8),(uint8_t)n};append(e,p,3);}
static void uuid_element(struct encoded *e,unsigned n,unsigned size)
{
    if(size==2){const uint8_t p[]={SDP_UUID16,(uint8_t)(n>>8),(uint8_t)n};append(e,p,3);}
    else if(size==4){const uint8_t p[]={SDP_UUID32,0,0,(uint8_t)(n>>8),(uint8_t)n};append(e,p,5);}
    else {const uint8_t p[]={SDP_UUID128,0,0,(uint8_t)(n>>8),(uint8_t)n,0,0,0x10,0,0x80,0,0,0x80,0x5f,0x9b,0x34,0xfb};append(e,p,17);}
}
static unsigned container(struct encoded *e,unsigned tag)
{unsigned start=e->n;octet(e,tag);octet(e,0);return start;}
static void end_container(struct encoded *e,unsigned start)
{assert(e->n-start-2<=255);e->bytes[start+1]=(uint8_t)(e->n-start-2);}
static struct encoded service(void)
{
    struct encoded e={0};unsigned outer=container(&e,SDP_SEQ8),record=container(&e,SDP_SEQ8);
    uint16_element(&e,SDP_ATTR_RECORD_HANDLE);const uint8_t handle[]={SDP_UINT32,0,1,0,1};append(&e,handle,5);
    uint16_element(&e,SDP_ATTR_SVCLASS_ID_LIST);unsigned classes=container(&e,SDP_SEQ8);
    uuid_element(&e,HANDSFREE_SVCLASS_ID,16);end_container(&e,classes);
    uint16_element(&e,SDP_ATTR_PROTO_DESC_LIST);unsigned alternatives=container(&e,SDP_ALT8);
    for(unsigned i=0;i<2;++i) {
        unsigned stack=container(&e,SDP_SEQ8),l2=container(&e,SDP_SEQ8);uuid_element(&e,L2CAP_UUID,i ? 16 : 2);
        if(i)uint16_element(&e,0x1001);end_container(&e,l2);
        unsigned rf=container(&e,SDP_SEQ8);uuid_element(&e,RFCOMM_UUID,i ? 2 : 4);octet(&e,SDP_UINT8);octet(&e,i ? 9 : 7);
        end_container(&e,rf);end_container(&e,stack);
    }
    end_container(&e,alternatives);
    uint16_element(&e,SDP_ATTR_PFILE_DESC_LIST);unsigned profiles=container(&e,SDP_SEQ8),profile=container(&e,SDP_SEQ8);
    uuid_element(&e,HANDSFREE_SVCLASS_ID,2);uint16_element(&e,0x0109);end_container(&e,profile);end_container(&e,profiles);
    uint16_element(&e,0x0100);const uint8_t text[]={SDP_TEXT_STR16,0,3,'A',0,'B'};append(&e,text,6);
    uint16_element(&e,SDP_ATTR_SUPPORTED_FEATURES);uint16_element(&e,0x003f);
    end_container(&e,record);end_container(&e,outer);return e;
}
static void peer_close(struct discovery *d)
{
    assert(bt_sdp_close(&d->client,UINT64_MAX)==1);
    unsigned out=doutput(d);const struct l2cap_cmd_hdr *cmd=command_out(&d->lower,out,L2CAP_DISCONN_REQ);
    struct l2cap_disconn_req endpoints={0x60,0x40};signal_in(&d->lower,L2CAP_DISCONN_RSP,cmd->ident,&endpoints,4);
    for(unsigned i=0;i<20 && d->client.phase!=BT_SDP_CLOSED;++i)dpoll(d);
}
static void continuation_and_lifetime(void)
{
    struct discovery d;dsetup(&d,1,10000000);srequest(&d,1,0,0);struct encoded e=service();
    uint8_t token[16];for(unsigned i=0;i<16;++i)token[i]=(uint8_t)(0xa0+i);
    d.lower.automatic_credits=0;srsp(&d,1,e.bytes,2,token,16);
    assert(d.client.status==BT_SDP_PENDING && d.client.length==2 && !bt_sdp_record(&d.client,0));
    srequest(&d,2,token,16);uint64_t tx=d.client.tx_serial;assert(tx && d.lower.a.credits==0);
    srsp(&d,1,e.bytes,2,token,16);assert(d.client.length==2);
    srsp(&d,2,e.bytes+2,e.n-2,0,0);
    const struct bt_sdp_record *r=bt_sdp_record(&d.client,0);
    assert(d.client.status==BT_SDP_COMPLETE && r && r->handle==0x10001 && r->matches_class && r->attributes==6 && r->endpoints==2);
    assert(r->endpoint[0].l2cap_psm==0 && r->endpoint[0].rfcomm_channel==7 &&
        r->endpoint[1].l2cap_psm==0x1001 && r->endpoint[1].rfcomm_channel==9);
    const struct bt_sdp_attribute *a=bt_sdp_attribute(&d.client,0,SDP_ATTR_SUPPORTED_FEATURES);
    const uint8_t feature[]={SDP_UINT16,0,0x3f};assert(a && a->length==3 && !memcmp(d.client.bytes+a->offset,feature,3));
    assert(d.client.tx_serial==tx && bt_sdp_release(&d.client)==-1);
    /* The continued request spans two 32-byte ACL packets. Retire its first
     * packet so the close request can use that credit; retain the second. */
    const uint8_t first_credit[]={1,0x23,0,1,0};event(&d.lower.f,HCI_EV_NUM_COMP_PKTS,first_credit,5);dpoll(&d);
    assert(d.client.tx_serial==tx && d.lower.a.credits==1);
    peer_close(&d);assert(d.client.phase==BT_SDP_CLOSING && d.client.tx_serial==tx);
    const uint8_t credit[]={1,0x23,0,2,0};event(&d.lower.f,HCI_EV_NUM_COMP_PKTS,credit,5);
    for(unsigned i=0;i<10 && d.client.phase!=BT_SDP_CLOSED;++i)dpoll(&d);
    assert(d.client.phase==BT_SDP_CLOSED && d.lower.a.credits==2 && bt_sdp_record(&d.client,0)==r);
    assert(bt_sdp_release(&d.client)==1 && !bt_sdp_record(&d.client,0));
}
static void expiry_and_late_response(void)
{
    struct discovery d;dsetup(&d,0,1000);srequest(&d,1,0,0);
    assert(d.client.tx_serial && d.lower.a.credits==0);d.lower.f.now=d.client.deadline;dpoll(&d);
    assert(d.client.status==BT_SDP_EXPIRED && d.client.phase==BT_SDP_RUNNING && d.client.tx_serial);
    struct encoded e=service();srsp(&d,1,e.bytes,e.n,0,0);
    assert(!d.client.records && !d.client.length && bt_sdp_release(&d.client)==-1);
}
static void malformed_and_remote_error(void)
{
    struct discovery d;dsetup(&d,1,10000000);srequest(&d,1,0,0);
    const uint8_t invalid[]={SDP_SEQ8,2,SDP_SEQ8,4};srsp(&d,1,invalid,4,0,0);
    assert(d.client.status==BT_SDP_INVALID && d.client.held_rx && !bt_sdp_record(&d.client,0));
    uint64_t held=d.client.held_rx;dpoll(&d);assert(d.client.held_rx==held);
    peer_close(&d);assert(d.client.phase==BT_SDP_CLOSED && !d.client.held_rx);
    dsetup(&d,1,10000000);srequest(&d,1,0,0);
    const uint8_t error[]={SDP_ERROR_RSP,0,1,0,2,0,5};drx(&d,error,sizeof(error));
    assert(d.client.status==BT_SDP_REMOTE_ERROR && d.client.remote_error==5 && !d.client.records);
}
static void bounded_continuation(void)
{
    struct discovery d;dsetup(&d,1,10000000);srequest(&d,1,0,0);
    const uint8_t bytes[]={SDP_SEQ8,0},token[]={0x55};
    for(unsigned i=1;i<=BT_SDP_SEGMENTS;++i) {
        srsp(&d,i,bytes,2,token,1);
        if(i<BT_SDP_SEGMENTS) {assert(d.client.status==BT_SDP_PENDING);srequest(&d,i+1,token,1);}
    }
    assert(d.client.status==BT_SDP_LIMIT && d.client.length==2*BT_SDP_SEGMENTS && d.client.held_rx && !d.client.records);
    dsetup(&d,1,10000000);srequest(&d,1,0,0);
    struct encoded e={0};unsigned starts[BT_SDP_DEPTH+1];
    for(unsigned i=0;i<BT_SDP_DEPTH+1;++i)starts[i]=container(&e,SDP_SEQ8);
    for(unsigned i=BT_SDP_DEPTH+1;i;--i)end_container(&e,starts[i-1]);srsp(&d,1,e.bytes,e.n,0,0);
    assert(d.client.status==BT_SDP_LIMIT && !d.client.records);
}
int main(void)
{
    continuation_and_lifetime();expiry_and_late_response();malformed_and_remote_error();bounded_continuation();
    puts("SDP actual UART/HCI/ACL/L2CAP: request bytes, opaque continuation, split elements, parsed advertisements, retained credits/close, expiry, malformed replies and finite bounds passed.");
    return 0;
}
