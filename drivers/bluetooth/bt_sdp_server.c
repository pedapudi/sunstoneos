/* SPDX-License-Identifier: Apache-2.0 */
#include "bt_sdp_server.h"
#include "bt_memory.h"
#include "a733_range.h"
static unsigned b16(const uint8_t *p){return ((unsigned)p[0]<<8)|p[1];}
static uint32_t b32(const uint8_t *p){return ((uint32_t)b16(p)<<16)|b16(p+2);}
static void p16(uint8_t *p,unsigned v){p[0]=(uint8_t)(v>>8);p[1]=(uint8_t)v;}
static void p32(uint8_t *p,uint32_t v){p16(p,v>>16);p16(p+2,v);}
static void cp(uint8_t *d,const uint8_t *s,size_t n){for(size_t i=0;i<n;++i)d[i]=s[i];}
static int eq(const uint8_t *a,const uint8_t *b,size_t n){for(size_t i=0;i<n;++i)if(a[i]!=b[i])return 0;return 1;}
/* Callers first require ACTIVE L2CAP. From then on the lower objects and
 * receive storage are fixed, since none of them can be initialized again in
 * this controller lifetime, so one successful check is latched. */
static int disjoint(struct bt_sdp_server *s)
{
    if(s->lower_checked)return 1;
    const struct bt_l2cap *l=s->l2cap;
    if(!bt_range_disjoint_empty(s,sizeof(*s),l,sizeof(*l)) || !bt_range_disjoint_empty(s,sizeof(*s),l->acl,sizeof(*l->acl)) ||
       !bt_range_disjoint_empty(s,sizeof(*s),l->acl->hci,sizeof(*l->acl->hci)) ||
       !bt_range_disjoint_empty(s,sizeof(*s),l->acl->hci->transport.binding.uart,sizeof(struct a733_uart)))return 0;
    for(unsigned i=0;i<l->acl->hci->transport.count;++i)
        if(!bt_range_disjoint_empty(s,sizeof(*s),l->acl->hci->transport.records[i].storage.bytes,l->acl->hci->transport.records[i].storage.capacity))return 0;
    s->lower_checked=1;return 1;
}
int bt_sdp_server_init(struct bt_sdp_server *s,struct bt_l2cap *l,const struct bt_sdp_ag_profile *p,
    uint64_t response,uint64_t query,int (*policy)(void *,uint64_t,uint16_t,unsigned),void *ctx)
{
    if(!s || !l || !p || !policy || s->initialized || l->state!=BT_L2CAP_OFF || !response || query<response ||
       !bt_range_disjoint_empty(s,sizeof(*s),l,sizeof(*l)) || !bt_range_disjoint_empty(s,sizeof(*s),p,sizeof(*p)) ||
       p->handle<0x10000 || !p->rfcomm_channel || p->rfcomm_channel>30 || p->version!=0x0109 ||
       p->supported_features>63 || p->supported_features!=((p->brsf_features&31)|((p->brsf_features>>4)&32)) ||
       p->network!=((p->brsf_features>>5)&1) || (p->brsf_features&~0x3ffu) ||
       (p->sink_handle && (p->sink_handle<0x10000 || p->sink_handle==p->handle || !p->sink_features)) ||
       p->sink_features>15 || (!p->sink_handle && p->sink_features))return -1;
    *s=(struct bt_sdp_server){.l2cap=l,.profile=*p,.policy=policy,.context=ctx,
        .response_us=response,.query_us=query,.next_query=1,.initialized=1};return 1;
}
int bt_sdp_server_admit(void *ctx,uint64_t link,uint16_t psm,unsigned incoming)
{
    struct bt_sdp_server *s=ctx;
    if(!s || !s->initialized || s->failed || s->l2cap->state!=BT_L2CAP_ACTIVE || !disjoint(s))return -1;
    int policy=s->policy(s->context,link,psm,incoming);
    if(psm!=1 || !incoming || policy!=1)return policy;
    struct bt_l2cap *l=s->l2cap;unsigned slot=BT_L2CAP_CHANNELS,free=0,live=0;
    for(unsigned i=0;i<BT_ACL_LINKS;++i)if(l->acl->links[i].generation==link &&
       l->acl->links[i].state==BT_ACL_CONNECTED && !l->acl->links[i].flush_seen)live=1;
    if(!live)return -1;
    if(l->next_cid>0xffff || l->next_channel==UINT64_MAX)return 0;
    for(unsigned i=0;i<BT_L2CAP_CHANNELS;++i) {
        if(l->channels[i].state==BT_L2CAP_UNUSED)free=1;
        if(s->sessions[i].channel && s->sessions[i].link==link)return 0;
        if(!s->sessions[i].channel && slot==BT_L2CAP_CHANNELS)slot=i;
    }
    if(!free || slot==BT_L2CAP_CHANNELS)return 0;
    s->sessions[slot]=(struct bt_sdp_server_session){.channel=l->next_channel,.link=link};return 1;
}
static struct bt_l2cap_channel *channel(struct bt_sdp_server *s,uint64_t identity)
{
    for(unsigned i=0;i<BT_L2CAP_CHANNELS;++i) {
        if(s->l2cap->channels[i].generation==identity && s->l2cap->channels[i].state!=BT_L2CAP_UNUSED)
            return &s->l2cap->channels[i];
    }
    return 0;
}
static struct bt_sdp_server_session *session(struct bt_sdp_server *s,uint64_t identity)
{for(unsigned i=0;i<BT_L2CAP_CHANNELS;++i)if(s->sessions[i].channel==identity && identity)return &s->sessions[i];return 0;}
struct de {unsigned type,at,end,length;};
static int de(const uint8_t *p,unsigned n,unsigned at,struct de *v)
{
    if(at>=n)return 0;unsigned tag=p[at++],type=tag>>3,size=tag&7,len;
    if(type==6) {
        if(size<5)return 0;unsigned w=1u<<(size-5);if(w>n-at)return 0;
        len=w==1?p[at]:w==2?b16(p+at):b32(p+at);at+=w;
    } else {
        if((type==3 && size!=1 && size!=2 && size!=4) || (type==1 && size!=1 && size!=2) ||
           (type!=1 && type!=3))return 0;
        len=1u<<size;
    }
    if(len>n-at)return 0;*v=(struct de){type,at,at+len,len};return 1;
}
static int uuid(const uint8_t *p,struct de v,unsigned target)
{
    static const uint8_t tail[]={0,0,0x10,0,0x80,0,0,0x80,0x5f,0x9b,0x34,0xfb};
    return v.type==3 && (v.length==2?b16(p+v.at)==target:
        b32(p+v.at)==target && (v.length==4 || eq(p+v.at+4,tail,12)));
}
/* All UUIDs actually contained in the immutable records: bit0 server,
 * bit1 AG, bit2 sink. MALFORMED is outside every record mask. */
#define MALFORMED 8u
static unsigned matches(const uint8_t *p,unsigned n,unsigned *at,unsigned sink)
{
    struct de list,v;if(!de(p,n,*at,&list) || list.type!=6 || !list.length)return MALFORMED;
    unsigned mask=sink?7:3,count=0;
    for(unsigned i=list.at;i<list.end;i=v.end) {
        if(!de(p,list.end,i,&v) || v.type!=3 || ++count>12)return MALFORMED;
        unsigned found=0;if(uuid(p,v,0x1000))found|=1;
        if(uuid(p,v,0x111f)||uuid(p,v,0x1203)||uuid(p,v,0x0100)||uuid(p,v,3)||
           uuid(p,v,0x111e)||uuid(p,v,0x1002))found|=2;
        if(uuid(p,v,0x110b)||uuid(p,v,0x0100)||uuid(p,v,0x0019)||uuid(p,v,0x110d)||uuid(p,v,0x1002))found|=4;
        mask&=found;
    }
    *at=list.end;return mask;
}
struct ranges {unsigned count;uint16_t lo[64],hi[64];};
static int ranges(const uint8_t *p,unsigned n,unsigned *at,struct ranges *r)
{
    struct de list,v;if(!de(p,n,*at,&list)||list.type!=6||!list.length)return 3;
    for(unsigned i=list.at;i<list.end;i=v.end) {
        if(!de(p,list.end,i,&v)||v.type!=1)return 3;
        if(r->count==64)return 6;
        unsigned low=b16(p+v.at),high=v.length==4?b16(p+v.at+2):low;
        if(low>high || (r->count && low<=r->hi[r->count-1]))return 3;
        r->lo[r->count]=(uint16_t)low;r->hi[r->count++]=(uint16_t)high;
    }
    *at=list.end;return 0;
}
static int selected(const struct ranges *r,unsigned id)
{for(unsigned i=0;i<r->count;++i)if(id>=r->lo[i]&&id<=r->hi[i])return 1;return 0;}
struct enc {uint8_t *p;unsigned n;};
static void add(struct enc *e,const uint8_t *p,unsigned n){cp(e->p+e->n,p,n);e->n+=n;}
static void u16(struct enc *e,unsigned v){const uint8_t p[]={9,(uint8_t)(v>>8),(uint8_t)v};add(e,p,3);}
static void u32(struct enc *e,uint32_t v){uint8_t p[5]={10};p32(p+1,v);add(e,p,5);}
static void attr(struct enc *e,const struct ranges *r,unsigned id,const uint8_t *p,unsigned n)
{if(selected(r,id)){u16(e,id);add(e,p,n);}}
enum {SERVER,AG,SINK};
static void record(struct enc *e,const struct ranges *r,const struct bt_sdp_ag_profile *p,unsigned kind)
{
    unsigned start=e->n,ag=kind==AG;const uint8_t seq[]={0x35,0};add(e,seq,2);
    if(selected(r,0)){u16(e,0);u32(e,kind==AG?p->handle:kind==SINK?p->sink_handle:0);}
    if(kind==SINK) {
        const uint8_t classes[]={0x35,3,0x19,0x11,0x0b};
        const uint8_t protocol[]={0x35,16,0x35,6,0x19,1,0,9,0,0x19,0x35,6,0x19,0,0x19,9,1,3};
        const uint8_t browse[]={0x35,3,0x19,0x10,2};
        const uint8_t profile[]={0x35,8,0x35,6,0x19,0x11,0x0d,9,1,3};
        const uint8_t features[]={9,0,(uint8_t)p->sink_features};
        attr(e,r,1,classes,sizeof(classes));attr(e,r,4,protocol,sizeof(protocol));attr(e,r,5,browse,sizeof(browse));
        attr(e,r,9,profile,sizeof(profile));attr(e,r,0x311,features,sizeof(features));
        e->p[start+1]=(uint8_t)(e->n-start-2);return;
    }
    const uint8_t server[]={0x35,3,0x19,0x10,0};
    const uint8_t classes[]={0x35,6,0x19,0x11,0x1f,0x19,0x12,3};
    attr(e,r,1,ag?classes:server,ag?sizeof(classes):sizeof(server));
    if(ag) {
        const uint8_t protocol[]={0x35,12,0x35,3,0x19,1,0,0x35,5,0x19,0,3,8,p->rfcomm_channel};
        const uint8_t browse[]={0x35,3,0x19,0x10,2};
        const uint8_t profile[]={0x35,8,0x35,6,0x19,0x11,0x1e,9,(uint8_t)(p->version>>8),(uint8_t)p->version};
        const uint8_t network[]={8,p->network};
        const uint8_t features[]={9,(uint8_t)(p->supported_features>>8),(uint8_t)p->supported_features};
        attr(e,r,4,protocol,sizeof(protocol));attr(e,r,5,browse,sizeof(browse));
        attr(e,r,9,profile,sizeof(profile));attr(e,r,0x301,network,sizeof(network));attr(e,r,0x311,features,sizeof(features));
    } else {const uint8_t version[]={0x35,3,9,1,0};attr(e,r,0x200,version,sizeof(version));}
    e->p[start+1]=(uint8_t)(e->n-start-2);
}
static void error(struct bt_sdp_server_session *q,unsigned tid,unsigned code)
{q->pending[0]=1;p16(q->pending+1,tid);p16(q->pending+3,2);p16(q->pending+5,code);q->pending_length=7;}
static void token(uint8_t *p,uint64_t query,unsigned offset)
{for(unsigned i=0;i<8;++i)p[i]=(uint8_t)(query>>(56-8*i));p32(p+8,offset);}
static void request(struct bt_sdp_server *s,struct bt_sdp_server_session *q,
 const struct bt_l2cap_channel *c,const struct bt_l2cap_pdu *rx)
{
    unsigned n=(unsigned)rx->length-4;const uint8_t *p=rx->bytes+4;
    q->held_rx=rx->serial;uint64_t now=s->l2cap->acl->hci->transport.now;
    if(!a733_deadline_after(now,s->response_us,&q->deadline)){q->error=BT_SDP_SERVER_EXPIRED;return;}
    if(n<5){q->error=BT_SDP_SERVER_SHORT;return;}
    unsigned tid=b16(p+1),kind=p[0],at=5,mask=0,maximum;
    if(b16(p+3)!=n-5){error(q,tid,4);return;}
    if(kind!=2 && kind!=4 && kind!=6){error(q,tid,3);return;}
    if(kind==4) {
        if(n-at<4){error(q,tid,4);return;}
        uint32_t handle=b32(p+at);at+=4;
        if(handle!=0 && handle!=s->profile.handle && (!s->profile.sink_handle || handle!=s->profile.sink_handle)){error(q,tid,2);return;}
        mask=!handle?1:handle==s->profile.handle?2:4;
    } else {mask=matches(p,n,&at,s->profile.sink_handle!=0);if(mask==MALFORMED){error(q,tid,3);return;}}
    if(n-at<2){error(q,tid,4);return;}maximum=b16(p+at);at+=2;
    if(maximum<(kind==2?1u:7u)){error(q,tid,3);return;}
    struct ranges r={0};if(kind!=2){unsigned e=ranges(p,n,&at,&r);if(e){error(q,tid,e);return;}}
    unsigned key_length=at-4;
    if(at==n || p[at]>16 || n-at!=1u+p[at]){error(q,tid,4);return;}
    if(p[at]) {
        uint8_t expected[12];token(expected,q->query,q->offset);
        if(p[at]!=12 || !q->query || !q->offset || q->offset>=q->result_length ||
           now>=q->query_deadline || q->request_length!=key_length || q->request[0]!=kind ||
           !eq(q->request+1,p+5,key_length-1)||!eq(p+at+1,expected,12)) {error(q,tid,5);return;}
    } else {
        if(s->next_query==UINT64_MAX || !a733_deadline_after(now,s->query_us,&q->query_deadline)){error(q,tid,6);return;}
        q->query=s->next_query++;q->offset=0;
        q->request[0]=(uint8_t)kind;cp(q->request+1,p+5,key_length-1);q->request_length=key_length;
        struct enc e={q->result,0};
        if(kind==2) {
            if(mask&1){p32(e.p+e.n,0);e.n+=4;}
            if((mask&2) && e.n/4<maximum){p32(e.p+e.n,s->profile.handle);e.n+=4;}
            if((mask&4) && e.n/4<maximum){p32(e.p+e.n,s->profile.sink_handle);e.n+=4;}
        } else {
            if(kind==6){e.p[e.n++]=0x35;e.p[e.n++]=0;}
            if(mask&1)record(&e,&r,&s->profile,SERVER);
            if(mask&2)record(&e,&r,&s->profile,AG);
            if(mask&4)record(&e,&r,&s->profile,SINK);
            if(kind==6)e.p[1]=(uint8_t)(e.n-2);
        }
        q->result_length=e.n;
    }
    uint8_t *out=q->pending;out[0]=(uint8_t)(kind+1);p16(out+1,tid);
    unsigned left=q->result_length-q->offset,bytes=left,head=kind==2?9:7;
    unsigned space=c->remote_mtu>BT_L2CAP_MTU?BT_L2CAP_MTU:c->remote_mtu;
    if(kind==2) {p16(out+5,q->result_length/4);p16(out+7,left/4);}
    else {if(bytes>maximum)bytes=maximum;if(bytes>space-head-1)bytes=space-head-1;}
    if(bytes<left) {
        if(bytes>space-head-13)bytes=space-head-13;
        if(left-bytes==1)--bytes;
    }
    if(kind!=2)p16(out+5,bytes);
    cp(out+head,q->result+q->offset,bytes);q->offset+=bytes;unsigned tail=head+bytes;
    out[tail++]=(uint8_t)(q->offset<q->result_length?12:0);
    if(q->offset<q->result_length){token(out+tail,q->query,q->offset);tail+=12;}
    p16(out+3,tail-5);q->pending_length=tail;
}
static void maintain(struct bt_sdp_server *s,struct bt_sdp_server_session *q)
{
    if(!q->channel)return;
    struct bt_l2cap_channel *c=channel(s,q->channel);
    if(!c || c->link!=q->link || c->psm!=1){q->error=BT_SDP_SERVER_IDENTITY;return;}
    if(q->tx_serial) {
        const struct bt_l2cap_pdu *p=0;
        for(unsigned i=0;i<BT_L2CAP_PDUS;++i)if(s->l2cap->tx[i].state!=BT_L2CAP_FREE &&
          s->l2cap->tx[i].serial==q->tx_serial)p=&s->l2cap->tx[i];
        if(!p || p->channel!=q->channel){q->error=BT_SDP_SERVER_IDENTITY;return;}
        if(p->state==BT_L2CAP_COMPLETED) {
            if(s->l2cap->acl->hci->transport.now>=q->deadline)q->error=BT_SDP_SERVER_EXPIRED;
            if(bt_l2cap_release_result(s->l2cap,p->serial)!=1){q->error=BT_SDP_SERVER_LOWER;return;}
            q->tx_serial=0;
        } else if(p->state==BT_L2CAP_FAILED) {
            q->error=BT_SDP_SERVER_LOWER;
            if(q->closing && !p->leases && bt_l2cap_release_result(s->l2cap,p->serial)==1)q->tx_serial=0;
        }
    }
    if((q->tx_serial || q->pending_length) && s->l2cap->acl->hci->transport.now>=q->deadline)
        q->error=BT_SDP_SERVER_EXPIRED;
    if(c->state==BT_L2CAP_QUARANTINED)q->error=BT_SDP_SERVER_LOWER;
    if(q->closing && !q->close_started && c->state!=BT_L2CAP_CLOSED) {
        if(s->l2cap->acl->hci->transport.now>=q->close_deadline){q->error=BT_SDP_SERVER_EXPIRED;return;}
        if(!q->tx_serial) {
            int rc=bt_l2cap_close(s->l2cap,q->channel,q->close_deadline);
            if(rc<0)q->error=BT_SDP_SERVER_LOWER;else if(rc==1)q->close_started=1;
        }
    }
}
int bt_sdp_server_step(struct bt_sdp_server *s,unsigned budget,uint64_t bound)
{
    if(!s || !s->initialized || s->failed)return -1;
    if(s->l2cap->state!=BT_L2CAP_ACTIVE || !disjoint(s) || bt_l2cap_step(s->l2cap,budget,bound)!=1){s->failed=1;return -1;}
    for(unsigned i=0;i<BT_L2CAP_CHANNELS;++i)maintain(s,&s->sessions[i]);
    for(unsigned i=0;i<BT_L2CAP_CHANNELS;++i) {
        unsigned slot=(s->rotation+i)%BT_L2CAP_CHANNELS;struct bt_sdp_server_session *q=&s->sessions[slot];
        if(!q->channel || q->error || q->closing)continue;
        struct bt_l2cap_channel *c=channel(s,q->channel);
        if(!c || c->state!=BT_L2CAP_OPEN || q->tx_serial)continue;
        s->rotation=(slot+1)%BT_L2CAP_CHANNELS;
        if(q->pending_length) {
            uint64_t now=s->l2cap->acl->hci->transport.now;
            if(s->policy(s->context,q->link,1,1)!=1){q->error=BT_SDP_SERVER_LOWER;return 1;}
            if(now>UINT64_MAX-s->l2cap->ertx_us){q->error=BT_SDP_SERVER_EXPIRED;return 1;}
            uint64_t serial;int rc=bt_l2cap_send(s->l2cap,q->channel,q->pending,q->pending_length,now+s->l2cap->ertx_us,&serial);
            if(rc<0){q->error=BT_SDP_SERVER_LOWER;return 1;}
            if(rc==1){q->tx_serial=serial;q->pending_length=0;
                if(bt_l2cap_release_receive(s->l2cap,q->held_rx)!=1)q->error=BT_SDP_SERVER_LOWER;
                else q->held_rx=0;}
            return 1;
        }
        const struct bt_l2cap_pdu *rx=0;
        for(unsigned j=0;j<BT_L2CAP_PDUS;++j){const struct bt_l2cap_pdu *p=&s->l2cap->rx[j];
            if(p->state==BT_L2CAP_RECEIVED && p->channel==q->channel && (!rx || p->serial<rx->serial))rx=p;}
        if(rx){request(s,q,c,rx);return 1;}
    }
    return 1;
}
int bt_sdp_server_close(struct bt_sdp_server *s,uint64_t identity,uint64_t bound)
{
    if(!s || !s->initialized || s->failed)return -1;struct bt_sdp_server_session *q=session(s,identity);
    if(!q || q->closing || bound<=s->l2cap->acl->hci->transport.now)return -1;
    q->closing=1;q->close_deadline=bound;q->pending_length=0;return 1;
}
int bt_sdp_server_release_channel(struct bt_sdp_server *s,uint64_t identity)
{
    if(!s || !s->initialized || s->failed)return -1;struct bt_sdp_server_session *q=session(s,identity);
    struct bt_l2cap_channel *c=channel(s,identity);
    if(!q || !c || c->state!=BT_L2CAP_CLOSED || c->completion!=BT_L2CAP_CONFIRMED || q->tx_serial)return -1;
    for(unsigned i=0;i<BT_L2CAP_PDUS;++i)if(s->l2cap->rx[i].state==BT_L2CAP_RECEIVED &&
      s->l2cap->rx[i].channel==identity && bt_l2cap_release_receive(s->l2cap,s->l2cap->rx[i].serial)!=1)return -1;
    int rc=bt_l2cap_release_channel(s->l2cap,identity);if(rc==1)*q=(struct bt_sdp_server_session){0};return rc;
}
