/* SPDX-License-Identifier: Apache-2.0 */
/* Original implementation of RFCOMM1.2 and its GSM07.10 basic-mode framing. */
#include "bt_rfcomm.h"
#include "bt_memory.h"
enum action { OTHER, MUX, PN, DLC, MSC, DISC, PEER_MSC, PEER_CLOSE, CREDIT, DATA, MUX_ACCEPT, PN_ACCEPT, DLC_ACCEPT };
static void copy(uint8_t *d,const uint8_t *s,size_t n){for(size_t i=0;i<n;++i)d[i]=s[i];}
static unsigned le16(const uint8_t *p){return p[0]|((unsigned)p[1]<<8);}
static uint64_t clock_now(const struct bt_rfcomm *q){return q->l2cap->acl->hci->transport.now;}
static int lower_disjoint(const struct bt_l2cap *l,const void *p,size_t n)
{
    if(!bt_range_disjoint_empty(p,n,l,sizeof(*l)))return 0;
    if(l->state==BT_L2CAP_OFF)return 1;
    if(!bt_range_disjoint_empty(p,n,l->acl,sizeof(*l->acl)) || !bt_range_disjoint_empty(p,n,l->acl->hci,sizeof(*l->acl->hci)) ||
       !bt_range_disjoint_empty(p,n,l->acl->hci->transport.binding.uart,sizeof(struct a733_uart)))return 0;
    for(unsigned i=0;i<l->acl->hci->transport.count;++i)
        if(!bt_range_disjoint_empty(p,n,l->acl->hci->transport.records[i].storage.bytes,
                   l->acl->hci->transport.records[i].storage.capacity))return 0;
    return 1;
}
static int external(const struct bt_rfcomm *q,const void *p,size_t n)
{
    if(!p || !bt_range_disjoint_empty(p,n,q,sizeof(*q)) || !bt_range_disjoint_empty(p,n,q->registry,sizeof(*q->registry)) ||
       !lower_disjoint(q->l2cap,p,n))return 0;
    for(unsigned i=0;i<BT_ACL_LINKS;++i)if(q->registry->session[i] &&
        !bt_range_disjoint_empty(p,n,q->registry->session[i],sizeof(*q)))return 0;
    return 1;
}
static const struct bt_l2cap_channel *channel(const struct bt_rfcomm *q)
{
    for(unsigned i=0;i<BT_L2CAP_CHANNELS;++i)if(q->l2cap->channels[i].generation==q->channel &&
        q->l2cap->channels[i].state!=BT_L2CAP_UNUSED)return &q->l2cap->channels[i];
    return 0;
}
static int live_link(const struct bt_l2cap *l,uint64_t link)
{
    for(unsigned i=0;i<BT_ACL_LINKS;++i)if(l->acl->links[i].generation==link &&
        l->acl->links[i].state==BT_ACL_CONNECTED && !l->acl->links[i].flush_seen)return 1;
    return 0;
}
static int authorized(const struct bt_rfcomm *q)
{return q->registry->authorize_service(q->registry->context,q->link,q->service_uuid,q->server)==1;}
static int fault(struct bt_rfcomm *q,enum bt_rfcomm_error e)
{q->phase=BT_RFCOMM_FAULT;if(q->error==BT_RFCOMM_NONE)q->error=e;return -1;}
int bt_rfcomm_registry_init(struct bt_rfcomm_registry *r,struct bt_l2cap *l,
    int (*psm)(void *,uint64_t,uint16_t,unsigned),int (*service)(void *,uint64_t,uint16_t,uint8_t),void *ctx)
{
    if(!r || !l || !psm || !service || r->l2cap || l->state!=BT_L2CAP_OFF ||
       !bt_range_disjoint_empty(r,sizeof(*r),l,sizeof(*l)))return -1;
    *r=(struct bt_rfcomm_registry){.l2cap=l,.authorize_psm=psm,.authorize_service=service,.context=ctx};return 1;
}
int bt_rfcomm_admit(void *p,uint64_t link,uint16_t psm,unsigned incoming)
{
    struct bt_rfcomm_registry *r=p;if(!r || !r->l2cap || !r->authorize_psm)return -1;
    if(psm!=3)return r->authorize_psm(r->context,link,psm,incoming);
    for(unsigned i=0;i<BT_ACL_LINKS;++i) {
        struct bt_rfcomm *q=r->session[i];if(!q || q->link!=link)continue;
        if(!incoming) {
            if(!q->incoming && q->phase==BT_RFCOMM_OFF && !q->channel)
                return r->authorize_psm(r->context,link,psm,0);
            return -1;
        }
        if(!q->incoming || q->phase!=BT_RFCOMM_LISTENING || q->channel || clock_now(q)>=q->deadline)return 0;
        for(unsigned k=0;k<BT_L2CAP_CHANNELS;++k)if(r->l2cap->channels[k].state!=BT_L2CAP_UNUSED &&
            r->l2cap->channels[k].link==link && r->l2cap->channels[k].psm==3)return 0;
        if(!live_link(r->l2cap,link) || !authorized(q))return -1;
        int allow=r->authorize_psm(r->context,link,psm,1);if(allow!=1)return allow;
        /* L2CAP publishes this generation synchronously after this callback.
         * A refused allocation creates no channel and grants no ownership. */
        q->admission=r->l2cap->next_channel;return 1;
    }
    return incoming ? 0 : -1;
}
int bt_rfcomm_begin(struct bt_rfcomm *q,struct bt_rfcomm_registry *r,const struct bt_sdp *s,
    unsigned record,unsigned endpoint,uint16_t mtu,uint64_t bound)
{
    if(!q || !r || !s || !r->l2cap || q->phase!=BT_RFCOMM_OFF || r->l2cap->state!=BT_L2CAP_ACTIVE ||
       r->l2cap->authorize!=bt_rfcomm_admit || r->l2cap->context!=r || s->l2cap!=r->l2cap ||
       mtu<23 || mtu>BT_RFCOMM_MTU || bound<=r->l2cap->acl->hci->transport.now ||
       !bt_range_disjoint_empty(q,sizeof(*q),r,sizeof(*r)) || !bt_range_disjoint_empty(q,sizeof(*q),s,sizeof(*s)) ||
       !lower_disjoint(r->l2cap,q,sizeof(*q)) || !lower_disjoint(r->l2cap,r,sizeof(*r)))return -1;
    const struct bt_sdp_record *rec=bt_sdp_record(s,record);
    if(!rec || !rec->matches_class || endpoint>=rec->endpoints)return -1;
    const struct bt_sdp_endpoint *ep=&rec->endpoint[endpoint];
    if(!ep->rfcomm_channel || ep->rfcomm_channel>30 || (ep->l2cap_psm && ep->l2cap_psm!=3) ||
       r->authorize_service(r->context,s->link,s->service_uuid,ep->rfcomm_channel)!=1)return -1;
    unsigned slot=BT_ACL_LINKS;
    for(unsigned i=0;i<BT_ACL_LINKS;++i) {
        if(!r->session[i]){if(slot==BT_ACL_LINKS)slot=i;continue;}
        if(r->session[i]->link==s->link || !bt_range_disjoint_empty(q,sizeof(*q),r->session[i],sizeof(*q)))return -1;
    }
    if(slot==BT_ACL_LINKS)return 0;
    for(unsigned i=0;i<BT_L2CAP_CHANNELS;++i)if(r->l2cap->channels[i].state!=BT_L2CAP_UNUSED &&
       r->l2cap->channels[i].link==s->link && r->l2cap->channels[i].psm==3)return -1;
    *q=(struct bt_rfcomm){.registry=r,.l2cap=r->l2cap,.link=s->link,.deadline=bound,.mtu=mtu,
        .server=ep->rfcomm_channel,.dlci=(uint8_t)(ep->rfcomm_channel*2),.service_uuid=s->service_uuid,.next_serial=1};
    r->session[slot]=q;
    uint64_t identity;int result=bt_l2cap_open(r->l2cap,q->link,3,bound,&identity);
    if(result!=1){r->session[slot]=0;*q=(struct bt_rfcomm){0};return result;}
    q->channel=identity;q->phase=BT_RFCOMM_OPENING;return 1;
}
int bt_rfcomm_listen(struct bt_rfcomm *q,struct bt_rfcomm_registry *r,uint64_t link,
    uint16_t uuid,uint8_t server,uint16_t mtu,uint64_t bound)
{
    if(!q || !r || !r->l2cap || q->phase!=BT_RFCOMM_OFF || r->l2cap->state!=BT_L2CAP_ACTIVE ||
       r->l2cap->authorize!=bt_rfcomm_admit || r->l2cap->context!=r || !uuid || !server || server>30 ||
       mtu<23 || mtu>BT_RFCOMM_MTU || bound<=r->l2cap->acl->hci->transport.now ||
       !bt_range_disjoint_empty(q,sizeof(*q),r,sizeof(*r)) || !lower_disjoint(r->l2cap,q,sizeof(*q)) ||
       !lower_disjoint(r->l2cap,r,sizeof(*r)))return -1;
    unsigned slot=BT_ACL_LINKS;
    for(unsigned i=0;i<BT_ACL_LINKS;++i) {
        if(!r->session[i]){if(slot==BT_ACL_LINKS)slot=i;continue;}
        if(r->session[i]->link==link || !bt_range_disjoint_empty(q,sizeof(*q),r->session[i],sizeof(*q)))return -1;
    }
    if(!live_link(r->l2cap,link) || r->authorize_service(r->context,link,uuid,server)!=1)return -1;
    if(slot==BT_ACL_LINKS)return 0;
    for(unsigned i=0;i<BT_L2CAP_CHANNELS;++i)if(r->l2cap->channels[i].state!=BT_L2CAP_UNUSED &&
        r->l2cap->channels[i].link==link && r->l2cap->channels[i].psm==3)return -1;
    *q=(struct bt_rfcomm){.registry=r,.l2cap=r->l2cap,.link=link,.deadline=bound,.mtu=mtu,
        .server=server,.dlci=(uint8_t)(server*2),.service_uuid=uuid,.next_serial=1,
        .incoming=1,.phase=BT_RFCOMM_LISTENING};r->session[slot]=q;return 1;
}
static int adopt(struct bt_rfcomm *q)
{
    if(!q->incoming || q->channel || !q->admission)return 0;
    for(unsigned i=0;i<BT_L2CAP_CHANNELS;++i) {
        const struct bt_l2cap_channel *c=&q->l2cap->channels[i];
        if(c->generation!=q->admission || c->state==BT_L2CAP_UNUSED)continue;
        if(c->psm!=3 || c->link!=q->link)return 0; /* Refused allocation may leave the number unused. */
        q->channel=c->generation;q->phase=BT_RFCOMM_OPENING;return 1;
    }
    return 0;
}
/* Reflected x^8+x^2+x+1, initial ones, complemented remainder. */
static uint8_t fcs(const uint8_t *p,unsigned n)
{
    unsigned r=255;
    for(unsigned i=0;i<n;++i){r^=p[i];for(unsigned b=0;b<8;++b)r=(r>>1)^((r&1) ? 0xe0u : 0u);}
    return (uint8_t)~r;
}
static unsigned length_put(uint8_t *p,unsigned n)
{p[0]=(uint8_t)((n<<1)|(n<128));if(n<128)return 1;p[1]=(uint8_t)(n>>7);return 2;}
static int length_get(const uint8_t *p,size_t n,unsigned *value,unsigned *width)
{
    if(!n)return 0;*value=p[0]>>1;*width=1;
    if(!(p[0]&1)){if(n<2)return 0;*value|=(unsigned)p[1]<<7;*width=2;}return 1;
}
static struct bt_rfcomm_buffer *free_tx(struct bt_rfcomm *q,unsigned data)
{
    unsigned first=data ? 0 : BT_RFCOMM_DATA,end=data ? BT_RFCOMM_DATA : BT_RFCOMM_SLOTS;
    for(unsigned i=first;i<end;++i)if(q->tx[i].state==BT_RFCOMM_FREE)return &q->tx[i];return 0;
}
static int frame(struct bt_rfcomm *q,unsigned dlci,unsigned control,unsigned cr,const uint8_t *p,
    unsigned n,unsigned grant,unsigned action,uint64_t deadline,uint64_t *serial)
{
    struct bt_rfcomm_buffer *b=free_tx(q,action==DATA);if(!b)return 0;
    if(q->next_serial==UINT64_MAX)return fault(q,BT_RFCOMM_EXHAUSTED);
    uint8_t bytes[BT_RFCOMM_MTU+6];bytes[0]=(uint8_t)((dlci<<2)|(cr<<1)|1);bytes[1]=(uint8_t)control;
    unsigned at=2+length_put(bytes+2,n),covered=(control&0xef)==0xef ? 2 : at;
    if(grant)bytes[at++]=(uint8_t)grant;
    copy(bytes+at,p,n);at+=n;bytes[at++]=fcs(bytes,covered);
    *b=(struct bt_rfcomm_buffer){.serial=q->next_serial++,.deadline=deadline,.length=at,.action=action,.grant=grant,.state=BT_RFCOMM_QUEUED};
    copy(b->bytes,bytes,at);if(serial)*serial=b->serial;return 1;
}
static int mcc(struct bt_rfcomm *q,unsigned type,unsigned command,const uint8_t *p,unsigned n,unsigned action)
{
    if(n>BT_RFCOMM_MTU-3)return fault(q,BT_RFCOMM_PROTOCOL);
    uint8_t bytes[BT_RFCOMM_MTU];bytes[0]=(uint8_t)(type|(command<<1)|1);
    unsigned at=1+length_put(bytes+1,n);copy(bytes+at,p,n);
    return frame(q,0,0xef,!q->incoming,bytes,at+n,0,action,a733_deadline_after_saturating(clock_now(q),q->l2cap->ertx_us),0);
}
static int start(struct bt_rfcomm *q)
{
    unsigned a=q->next_action;if(!a)return 1;
    struct bt_rfcomm_buffer *b=free_tx(q,0);if(!b)return 1;
    uint64_t serial=q->next_serial;int result;
    if(a==MUX || a==DLC || a==DISC)
        result=frame(q,a==MUX ? 0 : q->dlci,a==DISC ? 0x53 : 0x3f,!q->incoming,0,0,0,a,a733_deadline_after_saturating(clock_now(q),q->l2cap->ertx_us),0);
    else if(a==PN) {
        uint8_t p[8]={q->dlci,0xf0,7,0,(uint8_t)q->mtu,(uint8_t)(q->mtu>>8),0,BT_RFCOMM_DATA};
        result=mcc(q,0x80,1,p,8,a);if(result==1){b->grant=BT_RFCOMM_DATA;q->reserved_credits+=BT_RFCOMM_DATA;}
    } else {
        uint8_t p[2]={(uint8_t)((q->dlci<<2)|3),0x8d};result=mcc(q,0xe0,1,p,2,a);
    }
    if(result==1){q->request=serial;q->waiting=a;q->request_sent=0;q->timer=0;q->next_action=0;}
    return result<0 ? -1 : 1;
}
static void done(struct bt_rfcomm *q,unsigned next)
{q->request=0;q->waiting=0;q->request_sent=0;q->timer=0;q->next_action=next;}
static int eligible(const struct bt_rfcomm *q,const struct bt_l2cap_pdu *p,unsigned action)
{return q->waiting==action && q->request_sent && p->first_record>q->request_after;}
static int transmit_results(struct bt_rfcomm *q)
{
    for(unsigned i=0;i<BT_RFCOMM_SLOTS;++i) {
        struct bt_rfcomm_buffer *b=&q->tx[i];if(b->state!=BT_RFCOMM_SUBMITTED)continue;
        const struct bt_l2cap_pdu *p=0;
        for(unsigned j=0;j<BT_L2CAP_PDUS;++j)if(q->l2cap->tx[j].state!=BT_L2CAP_FREE && q->l2cap->tx[j].serial==b->lower_serial)p=&q->l2cap->tx[j];
        if(!p || p->channel!=q->channel)return fault(q,BT_RFCOMM_LOWER);
        if(!b->accepted && p->accepted==p->length) {
            b->accepted=1;
            if(b->grant>q->reserved_credits || b->grant>BT_RFCOMM_DATA-q->rx_credits)return fault(q,BT_RFCOMM_PROTOCOL);
            q->reserved_credits-=b->grant;
            for(unsigned g=0;g<b->grant;++g)q->credit_after[q->rx_credits++]=p->accepted_after;
            if(q->request==b->serial){q->request_sent=1;q->request_after=p->accepted_after;
                q->timer=a733_deadline_after_saturating(p->accepted_at,b->action==DLC ? 60000000 : 20000000);}
            if(b->action==PEER_MSC)q->peer_msc=1;
            if(b->action==MUX_ACCEPT){q->mux_open=1;q->mux_after=p->accepted_after;}
            if(b->action==PN_ACCEPT){q->pn_ready=1;q->pn_after=p->accepted_after;}
            if(b->action==DLC_ACCEPT){q->dlc_open=1;q->dlc_after=p->accepted_after;
                if(q->phase==BT_RFCOMM_NEGOTIATING)q->next_action=MSC;}
            if(b->action==PEER_CLOSE)q->wire_closed=1;
        }
        if(p->state==BT_L2CAP_COMPLETED || p->state==BT_L2CAP_FAILED) {
            unsigned failed=p->state==BT_L2CAP_FAILED;
            if(bt_l2cap_release_result(q->l2cap,p->serial)!=1)return fault(q,BT_RFCOMM_LOWER);
            b->lower_serial=0;
            if(b->action==DATA)b->state=failed ? BT_RFCOMM_CANCELLED : BT_RFCOMM_RETIRED;
            else *b=(struct bt_rfcomm_buffer){0};
            if(failed)return fault(q,BT_RFCOMM_LOWER);
        }
    }
    return 1;
}
static int submit(struct bt_rfcomm *q)
{
    struct bt_rfcomm_buffer *b=0;
    for(unsigned i=0;i<BT_RFCOMM_SLOTS;++i) {
        struct bt_rfcomm_buffer *t=&q->tx[i];if(t->state!=BT_RFCOMM_QUEUED)continue;
        if(t->action==DATA) {
            if(q->phase!=BT_RFCOMM_OPEN || clock_now(q)>=t->deadline){t->state=BT_RFCOMM_CANCELLED;continue;}
            if(!q->tx_credits)continue;
        }
        if(!b || t->serial<b->serial)b=t;
    }
    if(!b)return 1;
    uint64_t serial;int result=bt_l2cap_send(q->l2cap,q->channel,b->bytes,b->length,
        a733_deadline_after_saturating(clock_now(q),q->l2cap->ertx_us),&serial);
    if(result<0)return fault(q,BT_RFCOMM_LOWER);
    if(result==1){b->lower_serial=serial;b->state=BT_RFCOMM_SUBMITTED;if(b->action==DATA)--q->tx_credits;}
    return 1;
}
static int refuse_dlc(struct bt_rfcomm *q,unsigned dlci,unsigned pf)
{return frame(q,dlci,0x0f|(pf ? 0x10 : 0),q->incoming,0,0,0,OTHER,a733_deadline_after_saturating(clock_now(q),q->l2cap->ertx_us),0);}
static int control(struct bt_rfcomm *q,const struct bt_l2cap_pdu *rx,const uint8_t *p,unsigned n)
{
    if(q->incoming && (!q->mux_open || rx->first_record<=q->mux_after))return fault(q,BT_RFCOMM_PROTOCOL);
    unsigned size,w;if(n<2 || !(p[0]&1) || !length_get(p+1,n-1,&size,&w) || size!=n-1-w)return fault(q,BT_RFCOMM_PROTOCOL);
    unsigned type=p[0]&0xfc,command=(p[0]>>1)&1;const uint8_t *v=p+1+w;
    if(type==0x80) {
        if(size!=8)return fault(q,BT_RFCOMM_PROTOCOL);
        unsigned dlci=v[0]&63;
        if(command) {
            if(dlci!=q->dlci)return refuse_dlc(q,dlci,1);
            if(q->incoming && q->phase==BT_RFCOMM_NEGOTIATING && !q->pn_pending) {
                if(!q->mux_open || rx->first_record<=q->mux_after)return fault(q,BT_RFCOMM_PROTOCOL);
                if((v[1]&0xf0)!=0xf0)return fault(q,BT_RFCOMM_UNSUPPORTED);
                unsigned mtu=le16(v+4);if(mtu<23)return fault(q,BT_RFCOMM_PROTOCOL);
                if(mtu>q->mtu)mtu=q->mtu;
                struct bt_rfcomm_buffer *b=free_tx(q,0);if(!b)return 0;
                uint8_t answer[8]={(uint8_t)dlci,0xe0,(uint8_t)(v[2]&63),0,(uint8_t)mtu,(uint8_t)(mtu>>8),0,BT_RFCOMM_DATA};
                int result=mcc(q,type,0,answer,8,PN_ACCEPT);
                if(result==1){q->mtu=(uint16_t)mtu;q->tx_credits=v[7]&7;q->pn_pending=1;
                    b->grant=BT_RFCOMM_DATA;q->reserved_credits+=BT_RFCOMM_DATA;}return result;
            }
            /* Renegotiation cannot reset credit ownership or increase N1. */
            if(q->phase!=BT_RFCOMM_OPEN || (v[1]&0xf0) || le16(v+4)<23)return refuse_dlc(q,dlci,1);
            uint8_t answer[8];copy(answer,v,8);answer[0]=(uint8_t)dlci;answer[1]=0;answer[2]&=63;answer[3]=0;
            unsigned mtu=le16(v+4);if(mtu>q->mtu)mtu=q->mtu;
            /* A decrease would invalidate copied frames and retained capacity. */
            if(mtu!=q->mtu)return refuse_dlc(q,dlci,1);
            answer[4]=(uint8_t)mtu;answer[5]=(uint8_t)(mtu>>8);answer[6]=answer[7]=0;return mcc(q,type,0,answer,8,OTHER);
        }
        if(dlci!=q->dlci || !eligible(q,rx,PN))return 1;
        if((v[1]&0xf0)!=0xe0)return fault(q,BT_RFCOMM_UNSUPPORTED);
        unsigned mtu=le16(v+4);if(mtu<23 || mtu>q->mtu)return fault(q,BT_RFCOMM_PROTOCOL);
        q->mtu=(uint16_t)mtu;q->tx_credits=v[7]&7;done(q,DLC);return 1;
    }
    if(type==0xe0) {
        if((size!=2 && size!=3) || (v[0]&3)!=3 || !(v[1]&1) || (size==3 && !(v[2]&1)))return fault(q,BT_RFCOMM_PROTOCOL);
        if((v[0]>>2)!=q->dlci)return command ? refuse_dlc(q,v[0]>>2,1) : 1;
        if(command && (!q->dlc_open || (q->incoming && rx->first_record<=q->dlc_after)))return refuse_dlc(q,q->dlci,1);
        if(command) {int result=mcc(q,type,0,v,size,PEER_MSC);if(result==1)q->modem_status=v[1];return result;}
        if(eligible(q,rx,MSC)){if(size!=2 || (v[1]&~2u)!=0x8d)return fault(q,BT_RFCOMM_PROTOCOL);q->own_msc=1;done(q,0);}return 1;
    }
    if(!command)return 1;
    if(type==0x20)return mcc(q,type,0,v,size,OTHER);
    if(type==0x90) {
        if((size!=1 && size!=8) || (v[0]&3)!=3)return fault(q,BT_RFCOMM_PROTOCOL);
        if((v[0]>>2)!=q->dlci)return refuse_dlc(q,v[0]>>2,1);
        uint8_t a[8]={v[0],3,3,0,0x11,0x13,0x7f,0x3f};
        if(size==8) {
            unsigned mask=le16(v+6)&0x3f7f;
            if(v[1]!=3)mask&=~1u;
            if((v[2]&3)!=3)mask&=~2u;
            if(v[2]&4)mask&=~4u;
            if(v[2]&8)mask&=~8u;
            if(v[2]&0x30)mask&=~16u;
            if(v[4]!=0x11)mask&=~32u;if(v[5]!=0x13)mask&=~64u;
            mask&=~((unsigned)(v[3]&63)<<8);a[6]=(uint8_t)mask;a[7]=(uint8_t)(mask>>8);
        }
        return mcc(q,type,0,a,8,OTHER);
    }
    if(type==0x50) {
        if(size!=2 || (v[0]&3)!=3)return fault(q,BT_RFCOMM_PROTOCOL);
        if((v[0]>>2)!=q->dlci)return refuse_dlc(q,v[0]>>2,1);
        int result=mcc(q,type,0,v,size,OTHER);if(result==1)q->line_status=v[1]&15;return result;
    }
    if(type==0xa0 || type==0x60)return fault(q,BT_RFCOMM_UNSUPPORTED);
    if(type==0x10)return 1; /* Never answer NSC with another NSC. */
    return mcc(q,0x10,0,p,1,OTHER);
}
static void enter_close(struct bt_rfcomm *q,uint64_t bound)
{
    if(q->phase==BT_RFCOMM_CLOSING && q->close_deadline<bound)bound=q->close_deadline;
    q->phase=BT_RFCOMM_CLOSING;q->close_deadline=bound;done(q,0);
    for(unsigned i=0;i<BT_RFCOMM_SLOTS;++i) {
        struct bt_rfcomm_buffer *b=&q->tx[i];
        if(b->state!=BT_RFCOMM_QUEUED || b->action==PEER_CLOSE)continue;
        q->reserved_credits-=b->grant;
        if(b->action==DATA)b->state=BT_RFCOMM_CANCELLED;else *b=(struct bt_rfcomm_buffer){0};
    }
}
static int receive_frame(struct bt_rfcomm *q,const struct bt_l2cap_pdu *rx)
{
    const uint8_t *p=rx->bytes+4;size_t n=rx->length-4;
    unsigned length,w;if(n<4 || !(p[0]&1) || !length_get(p+2,n-2,&length,&w))return fault(q,BT_RFCOMM_PROTOCOL);
    unsigned dlci=p[0]>>2,cr=(p[0]>>1)&1,ctl=p[1]&0xef,pf=!!(p[1]&0x10),at=2+w;
    unsigned credit=ctl==0xef && dlci && pf;
    if(n!=at+credit+length+1 || p[n-1]!=fcs(p,ctl==0xef ? 2 : at))return fault(q,BT_RFCOMM_PROTOCOL);
    if(ctl!=0xef) {
        if(length || w!=1 || !pf)return fault(q,BT_RFCOMM_PROTOCOL);
        if(ctl==0x63 || ctl==0x0f) {
            if(cr!=!q->incoming)return fault(q,BT_RFCOMM_PROTOCOL);
            unsigned a=dlci==0 ? MUX : dlci==q->dlci ? q->waiting : OTHER;
            if(!a || !eligible(q,rx,a))return 1;
            if(ctl==0x0f && (a==MUX || a==PN || a==DLC || a==MSC || a==DISC))return fault(q,BT_RFCOMM_REFUSED);
            if(a!=MUX && a!=DLC && a!=DISC)return 1;
            if(a==MUX)done(q,PN);else if(a==DLC){q->dlc_open=1;done(q,MSC);}else {done(q,0);q->wire_closed=1;}
            return 1;
        }
        if(cr!=q->incoming)return fault(q,BT_RFCOMM_PROTOCOL);
        if(ctl==0x2f) {
            if(!q->incoming || q->phase!=BT_RFCOMM_NEGOTIATING)return refuse_dlc(q,dlci,1);
            unsigned action;
            if(!dlci && !q->mux_pending && !q->mux_open)action=MUX_ACCEPT;
            else if(dlci==q->dlci && q->pn_ready && rx->first_record>q->pn_after && !q->dlc_pending && !q->dlc_open)action=DLC_ACCEPT;
            else return refuse_dlc(q,dlci,1);
            int result=frame(q,dlci,0x73,1,0,0,0,action,a733_deadline_after_saturating(clock_now(q),q->l2cap->ertx_us),0);
            if(result==1){if(action==MUX_ACCEPT)q->mux_pending=1;else q->dlc_pending=1;}return result;
        }
        if(ctl!=0x43)return fault(q,BT_RFCOMM_PROTOCOL);
        if(dlci && dlci!=q->dlci)return refuse_dlc(q,dlci,1);
        int result=frame(q,dlci,0x73,q->incoming,0,0,0,PEER_CLOSE,a733_deadline_after_saturating(clock_now(q),q->l2cap->ertx_us),0);
        if(result==1)enter_close(q,a733_deadline_after_saturating(clock_now(q),20000000));return result;
    }
    if(cr!=q->incoming || (!dlci && pf))return fault(q,BT_RFCOMM_PROTOCOL);
    if(!dlci)return control(q,rx,p+at,length);
    if(dlci!=q->dlci)return refuse_dlc(q,dlci,pf);
    if((q->phase!=BT_RFCOMM_OPEN && !(q->phase==BT_RFCOMM_CLOSING && !q->wire_closed && q->dlc_open)) ||
       length>q->mtu-credit)return fault(q,BT_RFCOMM_PROTOCOL);
    unsigned grant=credit ? p[at++] : 0;
    if(grant>UINT32_MAX-q->tx_credits)return fault(q,BT_RFCOMM_PROTOCOL);
    struct bt_rfcomm_buffer *b=0;
    if(length) {
        unsigned credit_index=q->rx_credits;
        for(unsigned i=0;i<q->rx_credits;++i)if(rx->first_record>q->credit_after[i]){credit_index=i;break;}
        if(credit_index==q->rx_credits)return fault(q,BT_RFCOMM_PROTOCOL);
        for(unsigned i=0;i<BT_RFCOMM_DATA;++i)if(q->rx[i].state==BT_RFCOMM_FREE){b=&q->rx[i];break;}
        if(!b)return fault(q,BT_RFCOMM_PROTOCOL);
        if(q->next_serial==UINT64_MAX)return fault(q,BT_RFCOMM_EXHAUSTED);
        *b=(struct bt_rfcomm_buffer){.serial=q->next_serial++,.length=length,.state=BT_RFCOMM_RECEIVED};
        copy(b->bytes,p+at,length);--q->rx_credits;
        for(unsigned i=credit_index;i<q->rx_credits;++i)q->credit_after[i]=q->credit_after[i+1];
    }
    q->tx_credits+=grant;return 1;
}
static int close_lower(struct bt_rfcomm *q,const struct bt_l2cap_channel *c)
{
    if(clock_now(q)>=q->close_deadline)return fault(q,BT_RFCOMM_EXPIRED);
    unsigned submitted=0;
    for(unsigned i=0;i<BT_RFCOMM_SLOTS;++i)submitted+=q->tx[i].state==BT_RFCOMM_SUBMITTED;
    if(c->state==BT_L2CAP_OPEN && q->wire_closed && !q->closing_lower && !submitted) {
        int result=bt_l2cap_close(q->l2cap,q->channel,q->close_deadline);
        if(result<0)return fault(q,BT_RFCOMM_LOWER);if(result==1)q->closing_lower=1;
    }
    if(c->state==BT_L2CAP_CLOSED && c->completion==BT_L2CAP_CONFIRMED) {
        for(unsigned i=0;i<BT_RFCOMM_SLOTS;++i) {
            struct bt_rfcomm_buffer *b=&q->tx[i];
            if(b->state==BT_RFCOMM_SUBMITTED)return 1;
            if(b->state==BT_RFCOMM_QUEUED) {
                if(b->action==DATA)b->state=BT_RFCOMM_CANCELLED;else *b=(struct bt_rfcomm_buffer){0};
            }
        }
        int result=bt_l2cap_release_channel(q->l2cap,q->channel);
        if(result<0)return fault(q,BT_RFCOMM_LOWER);if(result==1){q->phase=BT_RFCOMM_CLOSED;return 0;}
    }
    return 1;
}
int bt_rfcomm_step(struct bt_rfcomm *q,unsigned budget,uint64_t bound)
{
    if(!q || q->phase==BT_RFCOMM_OFF || q->phase==BT_RFCOMM_FAULT)return -1;
    if(q->phase==BT_RFCOMM_CLOSED)return 0;
    if(!authorized(q))return fault(q,BT_RFCOMM_SECURITY);
    if(bt_l2cap_step(q->l2cap,budget,bound)!=1 || transmit_results(q)!=1)return fault(q,BT_RFCOMM_LOWER);
    if(q->incoming && !q->channel) {
        if(!adopt(q)) {
            if(clock_now(q)>=q->deadline){q->error=BT_RFCOMM_EXPIRED;q->phase=BT_RFCOMM_CLOSED;return 0;}
            return 1;
        }
    }
    const struct bt_l2cap_channel *c=channel(q);if(!c || c->link!=q->link)return fault(q,BT_RFCOMM_LOWER);
    if(c->state==BT_L2CAP_QUARANTINED)return fault(q,BT_RFCOMM_LOWER);
    if(q->phase==BT_RFCOMM_OPENING && c->state==BT_L2CAP_OPEN) {
        if(q->incoming) {
            unsigned cap=c->remote_mtu<c->local_mtu ? c->remote_mtu : c->local_mtu;
            if(cap<29)return fault(q,BT_RFCOMM_UNSUPPORTED);
            if(q->mtu>cap-6)q->mtu=(uint16_t)(cap-6);
        } else if(c->remote_mtu<q->mtu+6 || c->local_mtu<q->mtu+6)return fault(q,BT_RFCOMM_UNSUPPORTED);
        q->phase=BT_RFCOMM_NEGOTIATING;q->next_action=q->incoming ? 0 : MUX;
    }
    if(q->phase==BT_RFCOMM_NEGOTIATING && q->own_msc && q->peer_msc)q->phase=BT_RFCOMM_OPEN;
    uint64_t now=clock_now(q);
    if(q->phase==BT_RFCOMM_CLOSING && now>=q->close_deadline)return fault(q,BT_RFCOMM_EXPIRED);
    if(((q->phase==BT_RFCOMM_OPENING || q->phase==BT_RFCOMM_NEGOTIATING) && now>=q->deadline) ||
       (q->timer && now>=q->timer)) {
        q->error=BT_RFCOMM_EXPIRED;enter_close(q,a733_deadline_after_saturating(now,20000000));q->wire_closed=1;
    }
    const struct bt_l2cap_pdu *rx=0;
    for(unsigned i=0;i<BT_L2CAP_PDUS;++i) {
        const struct bt_l2cap_pdu *p=&q->l2cap->rx[i];
        if(p->state==BT_L2CAP_RECEIVED && p->channel==q->channel && (!rx || p->serial<rx->serial))rx=p;
    }
    if(rx) {
        int result=q->closing_lower ? 1 : receive_frame(q,rx);
        if(result<0){q->held_rx=rx->serial;return -1;}
        if(result==1 && bt_l2cap_release_receive(q->l2cap,rx->serial)!=1)return fault(q,BT_RFCOMM_LOWER);
    }
    if(c->state==BT_L2CAP_CLOSED && q->phase!=BT_RFCOMM_CLOSING)enter_close(q,a733_deadline_after_saturating(now,20000000));
    if(q->phase==BT_RFCOMM_CLOSING && (q->wire_closed || q->closing_lower || c->state==BT_L2CAP_CLOSED))return close_lower(q,c);
    if(q->phase==BT_RFCOMM_OPEN) {
        unsigned free=0;for(unsigned i=0;i<BT_RFCOMM_DATA;++i)free+=q->rx[i].state==BT_RFCOMM_FREE;
        if(free>q->rx_credits+q->reserved_credits) {
            unsigned grant=free-q->rx_credits-q->reserved_credits;
            int result=frame(q,q->dlci,0xff,!q->incoming,0,0,grant,CREDIT,a733_deadline_after_saturating(now,q->l2cap->ertx_us),0);
            if(result<0)return -1;if(result==1)q->reserved_credits+=grant;
        }
    }
    if(start(q)!=1)return -1;
    return c->state==BT_L2CAP_OPEN ? submit(q) : 1;
}
int bt_rfcomm_send(struct bt_rfcomm *q,const uint8_t *p,size_t n,uint64_t bound,uint64_t *serial)
{
    if(!q || q->phase!=BT_RFCOMM_OPEN || !n || n>q->mtu || bound<=clock_now(q) ||
       !external(q,p,n) || !external(q,serial,sizeof(*serial)) || !bt_range_disjoint_empty(p,n,serial,sizeof(*serial)) || !authorized(q))return -1;
    return frame(q,q->dlci,0xef,!q->incoming,p,(unsigned)n,0,DATA,bound,serial);
}
static const struct bt_rfcomm_buffer *oldest(const struct bt_rfcomm_buffer *p,unsigned n,unsigned received)
{
    const struct bt_rfcomm_buffer *b=0;
    for(unsigned i=0;i<n;++i)if((received ? p[i].state==BT_RFCOMM_RECEIVED :
       (p[i].state==BT_RFCOMM_RETIRED || p[i].state==BT_RFCOMM_CANCELLED)) && (!b || p[i].serial<b->serial))b=&p[i];
    return b;
}
const struct bt_rfcomm_buffer *bt_rfcomm_receive(const struct bt_rfcomm *q)
{return q ? oldest(q->rx,BT_RFCOMM_DATA,1) : 0;}
const struct bt_rfcomm_buffer *bt_rfcomm_result(const struct bt_rfcomm *q)
{return q ? oldest(q->tx,BT_RFCOMM_DATA,0) : 0;}
int bt_rfcomm_release_receive(struct bt_rfcomm *q,uint64_t serial)
{
    if(!q || !serial)return -1;
    for(unsigned i=0;i<BT_RFCOMM_DATA;++i)if(q->rx[i].serial==serial && q->rx[i].state==BT_RFCOMM_RECEIVED)
        {q->rx[i]=(struct bt_rfcomm_buffer){0};return 1;}
    return -1;
}
int bt_rfcomm_release_result(struct bt_rfcomm *q,uint64_t serial)
{
    if(!q || !serial)return -1;
    for(unsigned i=0;i<BT_RFCOMM_DATA;++i)if(q->tx[i].serial==serial &&
       (q->tx[i].state==BT_RFCOMM_RETIRED || q->tx[i].state==BT_RFCOMM_CANCELLED))
        {q->tx[i]=(struct bt_rfcomm_buffer){0};return 1;}
    return -1;
}
int bt_rfcomm_close(struct bt_rfcomm *q,uint64_t bound)
{
    if(!q || q->phase==BT_RFCOMM_OFF || q->phase==BT_RFCOMM_CLOSED || q->phase==BT_RFCOMM_CLOSING ||
       bound<=clock_now(q) || !authorized(q) || q->l2cap->state!=BT_L2CAP_ACTIVE)return -1;
    if(q->incoming && !q->channel && !adopt(q)) {q->phase=BT_RFCOMM_CLOSED;return 1;}
    unsigned normal=q->phase==BT_RFCOMM_OPEN;
    enter_close(q,bound);if(normal)q->next_action=DISC;else q->wire_closed=1;
    if(q->held_rx){if(bt_l2cap_release_receive(q->l2cap,q->held_rx)!=1)return fault(q,BT_RFCOMM_LOWER);q->held_rx=0;}
    return 1;
}
int bt_rfcomm_release(struct bt_rfcomm *q)
{
    if(!q || q->phase!=BT_RFCOMM_CLOSED)return -1;
    for(unsigned i=0;i<BT_RFCOMM_SLOTS;++i)if(q->tx[i].state!=BT_RFCOMM_FREE)return 0;
    for(unsigned i=0;i<BT_RFCOMM_DATA;++i)if(q->rx[i].state!=BT_RFCOMM_FREE)return 0;
    for(unsigned i=0;i<BT_ACL_LINKS;++i)if(q->registry->session[i]==q) {
        q->registry->session[i]=0;*q=(struct bt_rfcomm){0};return 1;
    }
    return -1;
}
