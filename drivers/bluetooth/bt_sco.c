/* SPDX-License-Identifier: Apache-2.0 */
/* Original implementation of Core5.4 HCI and HFP1.9 audio procedures. */
#include "bt_sco.h"
#include "bt_memory.h"
static unsigned le16(const uint8_t *p){return p[0]|((unsigned)p[1]<<8);}
static void put16(uint8_t *p,unsigned x){p[0]=(uint8_t)x;p[1]=(uint8_t)(x>>8);}
static void put32(uint8_t *p,unsigned x){put16(p,x);put16(p+2,x>>16);}
static int equal(const uint8_t *a,const uint8_t *b)
{for(unsigned i=0;i<6;++i)if(a[i]!=b[i])return 0;return 1;}
static uint64_t after(uint64_t now,uint64_t n){return n>UINT64_MAX-now ? UINT64_MAX : now+n;}
static struct bt_sco_link *link(struct bt_sco *q,uint64_t id)
{for(unsigned i=0;i<BT_SCO_LINKS;++i)if(id && q->links[i].generation==id)return &q->links[i];return 0;}
static struct bt_sco_link *handle(struct bt_sco *q,unsigned h)
{for(unsigned i=0;i<BT_SCO_LINKS;++i)if(q->links[i].handle_valid && q->links[i].handle==h)return &q->links[i];return 0;}
static struct bt_acl_link *parent(struct bt_sco *q,uint64_t id)
{for(unsigned i=0;i<BT_ACL_LINKS;++i)if(id && q->acl->links[i].generation==id)return &q->acl->links[i];return 0;}
static int fault(struct bt_sco *q,enum bt_sco_error e)
{if(!q->error)q->error=e;for(unsigned i=0;i<BT_SCO_LINKS;++i)if(q->links[i].phase!=BT_SCO_UNUSED && q->links[i].phase!=BT_SCO_CLOSED)q->links[i].phase=BT_SCO_RETAINED;return -1;}
static int valid(const struct bt_sco *q)
{return q && q->self==q && q->initialized && q->acl && q->acl->state==BT_ACL_ACTIVE &&
 q->acl->synchronous_context==q && q->acl->hci->self==q->acl->hci && q->acl->hci->acl_owner==q->acl &&
 q->acl->hci->synchronous_owner==q && q->acl->hci->synchronous_ready && !q->error;}
static int lower_external(const struct bt_acl *a,const void *p,size_t n)
{
    const struct bt_h4 *h=&a->hci->transport;
    if(!bt_range_disjoint(a,sizeof(*a),p,n) || !bt_range_disjoint(a->hci,sizeof(*a->hci),p,n) ||
       !bt_range_disjoint(h->binding.uart,sizeof(*h->binding.uart),p,n))return 0;
    for(unsigned i=0;i<h->count;++i)if(!bt_range_disjoint(h->records[i].storage.bytes,h->records[i].storage.capacity,p,n))return 0;
    for(unsigned i=0;i<BT_ACL_TX_SLOTS;++i)if(a->packets[i].state!=BT_ACL_TX_FREE &&
        !bt_range_disjoint(a->packets[i].bytes,a->packets[i].length,p,n))return 0;
    return 1;
}
static int ag_external(const struct bt_hfp_ag *a,const void *p,size_t n)
{
    if(!bt_range_disjoint(a,sizeof(*a),p,n) || !bt_range_disjoint(a->rfcomm,sizeof(*a->rfcomm),p,n) ||
       !bt_range_disjoint(a->registry,sizeof(*a->registry),p,n) || !bt_range_disjoint(a->rfcomm->l2cap,sizeof(*a->rfcomm->l2cap),p,n))return 0;
    for(unsigned i=0;i<BT_ACL_LINKS;++i)if(a->registry->session[i] &&
        !bt_range_disjoint(a->registry->session[i],sizeof(struct bt_rfcomm),p,n))return 0;
    return 1;
}
static int external(const struct bt_sco *q,const void *p,size_t n)
{
    if(!bt_range_disjoint(q,sizeof(*q),p,n) || !lower_external(q->acl,p,n))return 0;
    for(unsigned i=0;i<BT_SCO_LINKS;++i)if(q->links[i].phase!=BT_SCO_UNUSED && !ag_external(q->links[i].ag,p,n))return 0;
    return 1;
}
static int ag_current(struct bt_sco *q,const struct bt_hfp_ag *a,uint64_t generation,uint64_t channel,
                      struct bt_sco_security *security)
{
    if(!a || a->phase!=BT_HFP_AG_READY || !a->rfcomm || !a->registry ||
       a->link!=generation || a->channel!=channel || a->rfcomm->phase!=BT_RFCOMM_OPEN ||
       a->rfcomm->registry!=a->registry || a->rfcomm->link!=generation || a->rfcomm->channel!=channel ||
       !a->rfcomm->l2cap || a->rfcomm->l2cap->acl!=q->acl)return 0;
    struct bt_acl_link *p=parent(q,generation);
    if(!p || p->state!=BT_ACL_CONNECTED || !p->handle_valid || p->flush_seen)return 0;
    unsigned found=0;for(unsigned i=0;i<BT_ACL_LINKS;++i)found+=a->registry->session[i]==a->rfcomm;
    return found==1 && a->registry->authorize_service(a->registry->context,generation,a->rfcomm->service_uuid,a->rfcomm->server)==1 &&
        q->binding.security(q->binding.context,a,security)==1 && security->generation && security->secure_connections<=1;
}
static int prepare(void *ctx)
{
    struct bt_sco *q=ctx;if(!valid(q))return -1;
    struct bt_h4 *h=&q->acl->hci->transport;
    if(h->now<q->now || h->state!=BT_H4_ACTIVE)return fault(q,BT_SCO_LOWER);
    q->now=h->now;
    if(q->binding.current(q->binding.context,&h->binding,q->binding.scheduling_generation,q->binding.service_latency_us)!=1)
        return fault(q,BT_SCO_IDENTITY);
    for(unsigned i=0;i<BT_SCO_LINKS;++i){struct bt_sco_link *l=&q->links[i];
        if(l->phase==BT_SCO_UNUSED || l->phase==BT_SCO_CLOSED)continue;
        struct bt_sco_security s={0};
        if(!ag_current(q,l->ag,l->acl_generation,l->channel,&s) || s.generation!=l->security.generation ||
           s.secure_connections!=l->security.secure_connections)return fault(q,BT_SCO_SECURITY);
        /* An open link keeps the codec bound at open. A cleared selection only
         * requires reselection before the next open; a different one faults. */
        if(l->phase==BT_SCO_OPEN&&l->ag->local_codecs&&(l->ag->hf_features&128)&&
           l->ag->selected_codec&&l->ag->selected_codec!=l->codec)return fault(q,BT_SCO_IDENTITY);
        if(l->phase!=BT_SCO_OPEN && q->now>=l->deadline)return fault(q,BT_SCO_EXPIRED);
    }
    /* No other submitter can replace the H4 payload before this post-step
     * observation. ACL invokes prepare before and after its HCI step. */
    if(q->active_tx<BT_SCO_PACKETS && h->tx_offset==h->tx_length){
        struct bt_sco_packet *t=&q->tx[q->active_tx];
        if(h->tx_payload!=t->bytes || t->state!=BT_SCO_PARTIAL)return fault(q,BT_SCO_IDENTITY);
        t->state=BT_SCO_SENT;q->active_tx=BT_SCO_PACKETS;
    }
    for(unsigned i=0;i<BT_SCO_PACKETS;++i){struct bt_sco_packet *t=&q->tx[i];
        if(t->state==BT_SCO_QUEUED && q->now>=t->deadline)t->state=BT_SCO_NOT_SENT;
        else if((t->state==BT_SCO_SENT || t->state==BT_SCO_PARTIAL) && q->now>=t->deadline)return fault(q,BT_SCO_EXPIRED);
    }
    return 1;
}
static int acknowledgement_pending(void *ctx)
{
    struct bt_sco *q=ctx;
    if(q->refuse==2)return 1;
    if(q->command_link>=BT_SCO_LINKS)return 0;
    enum bt_sco_phase p=q->links[q->command_link].phase;
    return p==BT_SCO_FEATURE_STATUS || p==BT_SCO_SETUP_STATUS || p==BT_SCO_REJECT_STATUS || p==BT_SCO_DISCONNECT_STATUS;
}
static int owns(void *ctx,uint16_t h){return handle(ctx,h)!=0;}
static int credits_valid(void *ctx,const uint8_t *entries,unsigned n)
{
    struct bt_sco *q=ctx;unsigned total=0;
    if(!valid(q) || q->credits>q->capacity)return -1;
    for(unsigned i=0;i<n;++i){const uint8_t *p=entries+4*i;struct bt_sco_link *l=handle(q,le16(p));
        if(!l)continue;
        if(l->phase!=BT_SCO_OPEN && l->phase!=BT_SCO_DISCONNECT_QUEUED &&
           l->phase!=BT_SCO_DISCONNECT_STATUS && l->phase!=BT_SCO_DISCONNECT_WAIT)return -1;
        unsigned owned=0;for(unsigned j=0;j<BT_SCO_PACKETS;++j)
            owned+=q->tx[j].state==BT_SCO_SENT && q->tx[j].link==l->generation;
        unsigned count=le16(p+2);if(count>owned)return -1;total+=count;
    }
    return total<=q->capacity-q->credits ? 1 : -1;
}
static void credits_commit(void *ctx,const uint8_t *entries,unsigned n)
{
    struct bt_sco *q=ctx;
    for(unsigned i=0;i<n;++i){const uint8_t *p=entries+4*i;struct bt_sco_link *l=handle(q,le16(p));
        if(!l)continue;
        for(unsigned c=le16(p+2);c;--c){struct bt_sco_packet *oldest=0;
            for(unsigned j=0;j<BT_SCO_PACKETS;++j){struct bt_sco_packet *t=&q->tx[j];
                if(t->state==BT_SCO_SENT && t->link==l->generation && (!oldest || t->serial<oldest->serial))oldest=t;}
            oldest->state=BT_SCO_RETIRED;++q->credits;
        }
    }
}
static int disconnect_valid(void *ctx,const struct bt_h4_record *r)
{
    struct bt_sco *q=ctx;const uint8_t *p=r->storage.bytes;struct bt_sco_link *l=handle(q,le16(p+3));
    if(!valid(q) || !l || (l->phase!=BT_SCO_OPEN && l->phase!=BT_SCO_DISCONNECT_QUEUED && l->phase!=BT_SCO_DISCONNECT_WAIT))return -1;
    if(l->phase==BT_SCO_DISCONNECT_WAIT && r->serial<=l->acknowledgement)return -1;
    if(p[2])return l->phase==BT_SCO_DISCONNECT_WAIT ? 1 : -1;
    unsigned sent=0;for(unsigned i=0;i<BT_SCO_PACKETS;++i)if(q->tx[i].link==l->generation){
        if(q->tx[i].state==BT_SCO_PARTIAL)return -1;sent+=q->tx[i].state==BT_SCO_SENT;}
    return q->credits<=q->capacity && sent<=q->capacity-q->credits ? 1 : -1;
}
static void disconnect_commit(void *ctx,const struct bt_h4_record *r)
{
    struct bt_sco *q=ctx;const uint8_t *p=r->storage.bytes;struct bt_sco_link *l=handle(q,le16(p+3));
    l->status=p[2];l->reason=p[5];
    if(p[2])l->phase=BT_SCO_OPEN;
    else {l->phase=BT_SCO_CLOSED;l->closed_after=r->serial;
        for(unsigned i=0;i<BT_SCO_PACKETS;++i){struct bt_sco_packet *t=&q->tx[i];if(t->link!=l->generation)continue;
            if(t->state==BT_SCO_SENT){t->state=BT_SCO_FLUSHED;++q->credits;}
            else if(t->state==BT_SCO_QUEUED)t->state=BT_SCO_NOT_SENT;
        }
    }
    if(q->command_link<BT_SCO_LINKS && &q->links[q->command_link]==l)q->command_link=BT_SCO_LINKS;
}
static int release(struct bt_sco *q,const struct bt_h4_record *r)
{return bt_hci_release(q->acl->hci,r->serial)==1 ? 1 : fault(q,BT_SCO_LOWER);}
/* Profiles: S1, S4, D1, D0. HFP1.9 §6.7; zero bit enables EDR packets. */
static unsigned profile(struct bt_sco *q,struct bt_sco_link *l,unsigned type)
{
    const uint8_t *local=q->acl->hci->identity.features;
    if(l->codec==2){
        if(type!=2 || !(local[2]&l->features[2]&8) || !(local[3]&l->features[3]&0x80))return 0;
        if(local[5]&l->features[5]&0x20)return 6;
        return l->security.secure_connections?0:5;
    }
    if(!(l->features[1]&8) || !(l->features[2]&1))return 0;
    if(type==2 && (local[3]&l->features[3]&0x80)){
        if(l->security.secure_connections)return (local[5]&l->features[5]&0x20) ? 2 : 0;
        return 1;
    }
    if(type==0 && !l->security.secure_connections)return (local[1]&l->features[1]&0x20) ? 3 : 4;
    return 0;
}
static int completed_failure(struct bt_sco *q,struct bt_sco_link *l,unsigned status)
{
    l->status=(uint8_t)status;q->command_link=BT_SCO_LINKS;
    /* Retry only the finite HFP transport fallback after an actual terminal
     * setup event. Command refusal and timeout do not enter this path. */
    if(l->codec==2){
        if(!l->listening&&l->phase==BT_SCO_SETUP_WAIT&&l->profile==6&&!l->security.secure_connections){
            l->profile=5;l->phase=BT_SCO_SETUP_QUEUED;
        }else {l->phase=BT_SCO_CLOSED;l->fallback_allowed=1;}
    }else if(!l->listening && l->phase==BT_SCO_SETUP_WAIT && !l->security.secure_connections &&
       (l->profile==1 || l->profile==3)){
        l->profile=l->profile==1 ? profile(q,l,0) : 4;
        l->phase=l->profile ? BT_SCO_SETUP_QUEUED : BT_SCO_CLOSED;
    }else l->phase=BT_SCO_CLOSED;
    return 1;
}
static int synchronous_event(void *ctx,const struct bt_h4_record *r)
{
    struct bt_sco *q=ctx;if(!valid(q))return -1;
    const uint8_t *p=r->storage.bytes;
    if(r->type==3){
        if(r->length<3 || r->length!=3u+p[2] || (le16(p)&0xc000))return fault(q,BT_SCO_PROTOCOL);
        struct bt_sco_link *l=handle(q,le16(p)&0xfff);
        if(!l)return 0;
        unsigned held=0;
        for(unsigned i=0;i<BT_H4_SLOTS;++i)held|=q->held_rx[i]==r->serial;
        if(l->phase!=BT_SCO_OPEN && l->phase!=BT_SCO_DISCONNECT_QUEUED && l->phase!=BT_SCO_DISCONNECT_STATUS && l->phase!=BT_SCO_DISCONNECT_WAIT &&
           !(l->phase==BT_SCO_CLOSED && held && r->serial<l->closed_after))return fault(q,BT_SCO_PROTOCOL);
        if(q->next_packet==UINT64_MAX)return fault(q,BT_SCO_IDENTITY);
        unsigned slot=BT_H4_SLOTS,earlier=0;
        for(unsigned i=0;i<q->acl->hci->transport.count;++i){
            if(&q->acl->hci->transport.records[i]==r)slot=i;
            if(q->held_rx[i] && q->held_rx[i]<r->serial)earlier=1;
        }
        if(slot==BT_H4_SLOTS)return fault(q,BT_SCO_IDENTITY);
        if(!earlier)for(unsigned i=0;i<BT_SCO_PACKETS;++i)if(q->rx[i].state==BT_SCO_FREE){
            struct bt_sco_packet *t=&q->rx[i];*t=(struct bt_sco_packet){.serial=q->next_packet++,
                .link=l->generation,.acquired=r->serial,.length=p[2],.status=(le16(p)>>12)&3,.state=BT_SCO_RECEIVED};
            for(unsigned j=0;j<p[2];++j)t->bytes[j]=p[3+j];q->held_rx[slot]=0;return release(q,r);
        }
        q->held_rx[slot]=r->serial;return 0;
    }
    if(r->type!=4)return 0;
    unsigned event=p[0],n=p[1];
    if(event==0x0f || event==0x0e){
        if(q->refuse==2 && le16(p+(event==0x0f ? 4 : 3))==0x042a && r->tag==q->refuse_tag){
            if(q->acl->hci->command!=BT_HCI_IDLE)return fault(q,BT_SCO_PROTOCOL);
            /* A refused reject means the controller already ended the request. */
            q->refuse=p[event==0x0f ? 2 : 5] ? 0 : 3;q->refuse_acknowledgement=r->serial;return release(q,r);
        }
        if(q->command_link>=BT_SCO_LINKS)return 0;
        struct bt_sco_link *l=&q->links[q->command_link];
        unsigned op=le16(p+(event==0x0f ? 4 : 3));
        unsigned expected=l->phase==BT_SCO_FEATURE_STATUS ? 0x041b :
            l->phase==BT_SCO_SETUP_STATUS ? (l->listening ? 0x043e : 0x043d) :
            l->phase==BT_SCO_REJECT_STATUS ? 0x042a : l->phase==BT_SCO_DISCONNECT_STATUS ? 0x0406 : 0;
        if(!expected || op!=expected || r->tag!=l->command_tag)return 0;
        if(q->acl->hci->command!=BT_HCI_IDLE)return fault(q,BT_SCO_PROTOCOL);
        l->status=p[event==0x0f ? 2 : 5];l->acknowledgement=r->serial;
        if(l->status){
            l->phase=l->phase==BT_SCO_DISCONNECT_STATUS ? BT_SCO_OPEN : BT_SCO_CLOSED;
            q->command_link=BT_SCO_LINKS;
        }else ++l->phase; /* Each STATUS enum is followed by its WAIT. */
        return release(q,r);
    }
    if(event==0x0b){
        if(n!=11)return fault(q,BT_SCO_PROTOCOL);
        if(q->command_link>=BT_SCO_LINKS)return 0;
        struct bt_sco_link *l=&q->links[q->command_link];
        if(le16(p+3)!=l->acl_handle || (l->phase!=BT_SCO_FEATURE_WAIT && l->phase!=BT_SCO_FEATURE_STATUS))return 0;
        if(l->phase!=BT_SCO_FEATURE_WAIT || r->serial<=l->acknowledgement)return fault(q,BT_SCO_PROTOCOL);
        q->command_link=BT_SCO_LINKS;l->status=p[2];
        if(p[2])l->phase=BT_SCO_CLOSED;
        else {for(unsigned i=0;i<8;++i)l->features[i]=p[5+i];
            l->profile=profile(q,l,l->requested ? l->request_type :
                (l->features[3]&q->acl->hci->identity.features[3]&0x80) ? 2 : 0);
            if(!l->profile){
                if(l->requested){l->reason=0x0e;l->phase=BT_SCO_REJECT_QUEUED;}
                else {l->phase=BT_SCO_CLOSED;l->status=0x11;l->fallback_allowed=l->codec==2;}
            }else l->phase=l->listening && !l->requested ? BT_SCO_LISTENING : BT_SCO_SETUP_QUEUED;
        }
        return release(q,r);
    }
    if(event==0x04){
        if(n!=10)return fault(q,BT_SCO_PROTOCOL);
        if(p[11]==1)return 0;
        if(p[11]!=0 && p[11]!=2)return fault(q,BT_SCO_PROTOCOL);
        struct bt_sco_link *l=0;
        for(unsigned i=0;i<BT_SCO_LINKS;++i)if(q->links[i].phase!=BT_SCO_UNUSED && equal(q->links[i].address,p+2))l=&q->links[i];
        if(!l || !l->listening || l->requested || (l->phase!=BT_SCO_LISTENING &&
            l->phase!=BT_SCO_FEATURE_QUEUED && l->phase!=BT_SCO_FEATURE_STATUS && l->phase!=BT_SCO_FEATURE_WAIT)){
            /* No link expects this request; it never consumes a link identity.
             * A repeat for an already requested link is not refused, since
             * the controller would apply the refusal to the pending request. */
            if(!q->refuse && !(l && l->listening && l->requested)){for(unsigned j=0;j<6;++j)q->refuse_address[j]=p[2+j];
                q->refuse_deadline=after(q->now,q->binding.response_timeout_us);q->refuse=1;}
            return release(q,r);
        }
        if(bt_hci_incoming_bound(q->acl->hci,q->binding.response_timeout_us)!=1)return fault(q,BT_SCO_IDENTITY);
        l->requested=1;l->request_type=p[11];l->request_serial=r->serial;
        l->request_deadline=after(q->now,q->binding.response_timeout_us);
        if(l->deadline>l->request_deadline)l->deadline=l->request_deadline;
        if(l->phase==BT_SCO_LISTENING){
            l->profile=profile(q,l,p[11]);
            if(!l->profile){l->reason=0x0e;l->phase=BT_SCO_REJECT_QUEUED;}
            else l->phase=BT_SCO_SETUP_QUEUED;
        }
        return release(q,r);
    }
    if(event==0x2c){
        if(n!=17)return fault(q,BT_SCO_PROTOCOL);
        if(q->refuse==3 && p[2] && equal(q->refuse_address,p+5) && r->serial>q->refuse_acknowledgement){
            q->refuse=0;return release(q,r);}
        if(q->command_link>=BT_SCO_LINKS)return p[2] ? 0 : fault(q,BT_SCO_IDENTITY);
        struct bt_sco_link *l=&q->links[q->command_link];
        if(!equal(l->address,p+5))return p[2] ? 0 : fault(q,BT_SCO_IDENTITY);
        if((l->phase!=BT_SCO_SETUP_WAIT && l->phase!=BT_SCO_REJECT_WAIT) || r->serial<=l->acknowledgement)return fault(q,BT_SCO_PROTOCOL);
        if(p[2]){completed_failure(q,l,p[2]);return release(q,r);}
        unsigned h=le16(p+3),type=l->profile<=2||l->codec==2 ? 2 : 0;
        if(l->phase==BT_SCO_REJECT_WAIT || h>0xeff || handle(q,h) || p[11]!=type || p[18]!=(l->codec==2?3:2))return fault(q,BT_SCO_PROTOCOL);
        for(unsigned i=0;i<BT_ACL_LINKS;++i)if(q->acl->links[i].handle_valid && q->acl->links[i].handle==h)return fault(q,BT_SCO_IDENTITY);
        if((!type && (p[12] || p[13] || le16(p+14) || le16(p+16))) ||
           (type && (!p[12] || !le16(p+14) || !le16(p+16))))return fault(q,BT_SCO_PROTOCOL);
        l->handle=(uint16_t)h;l->handle_valid=1;l->type=p[11];l->interval=p[12];l->retransmission=p[13];
        l->rx_packet_length=(uint16_t)le16(p+14);l->tx_packet_length=(uint16_t)le16(p+16);
        l->phase=BT_SCO_OPEN;l->status=0;q->command_link=BT_SCO_LINKS;return release(q,r);
    }
    if(event==0x2d){
        if(n!=9)return fault(q,BT_SCO_PROTOCOL);
        struct bt_sco_link *l=handle(q,le16(p+3));if(!l)return 0;
        /* No local renegotiation is outstanding. Successful remote eSCO
         * renegotiation preserves codec/HCI path; record its actual schedule. */
        if(l->phase!=BT_SCO_OPEN || l->type!=2)return fault(q,BT_SCO_PROTOCOL);
        if(!p[2]){if(!p[5] || !le16(p+7) || !le16(p+9))return fault(q,BT_SCO_PROTOCOL);
            l->interval=p[5];l->retransmission=p[6];l->rx_packet_length=(uint16_t)le16(p+7);l->tx_packet_length=(uint16_t)le16(p+9);}
        l->status=p[2];return release(q,r);
    }
    return 0;
}
static const struct bt_acl_synchronous_ops operations={BT_SCO_LINKS,prepare,acknowledgement_pending,owns,credits_valid,credits_commit,disconnect_valid,disconnect_commit,synchronous_event};
int bt_sco_init(struct bt_sco *q,struct bt_acl *a,const struct bt_sco_binding *b)
{
    if(!q || !a || !b || q->initialized || q->self || a->state!=BT_ACL_ACTIVE || !a->hci ||
       a->hci->self!=a->hci || a->hci->acl_owner!=a || !a->hci->synchronous_ready || a->synchronous ||
       !b->current || !b->security || !b->scheduling_generation || !b->service_latency_us ||
       !b->response_timeout_us || b->response_timeout_us>60000000 ||
       !lower_external(a,q,sizeof(*q)) || !lower_external(a,b,sizeof(*b)) || !bt_range_disjoint(q,sizeof(*q),b,sizeof(*b)))return -1;
    struct bt_hci *h=a->hci;
    if(!bt_hci_synchronous_supported(h) || bt_hci_incoming_bound(h,b->response_timeout_us)!=1 ||
       b->current(b->context,&h->transport.binding,b->scheduling_generation,b->service_latency_us)!=1)return -1;
    if(bt_acl_synchronous_register(a,&operations,q)!=1)return -1;
    *q=(struct bt_sco){.self=q,.acl=a,.binding=*b,.next_link=1,.next_packet=1,.now=h->transport.now,
        .capacity=le16(h->identity.buffer_size+5),.credits=le16(h->identity.buffer_size+5),
        .mtu=h->identity.buffer_size[2],.command_link=BT_SCO_LINKS,.active_tx=BT_SCO_PACKETS,.initialized=1};return 1;
}
int bt_sco_open(struct bt_sco *q,const struct bt_hfp_ag *ag,unsigned listening,uint64_t deadline,uint64_t *id)
{
    if(!valid(q) || !ag || listening>1 || !external(q,ag,sizeof(*ag)) || !external(q,id,sizeof(*id)) ||
       !bt_range_disjoint(ag,sizeof(*ag),id,sizeof(*id)) || deadline<=q->acl->hci->transport.now)return -1;
    struct bt_sco_security s={0};
    if((ag->local_codecs&&(ag->hf_features&128)&&!ag->selected_codec)||
       !ag_current(q,ag,ag->link,ag->channel,&s) || !ag_external(ag,q,sizeof(*q)) || !ag_external(ag,id,sizeof(*id)))return -1;
    struct bt_acl_link *p=parent(q,ag->link);
    for(unsigned i=0;i<BT_SCO_LINKS;++i)if(q->links[i].phase!=BT_SCO_UNUSED && equal(q->links[i].address,p->peer.address))return -1;
    if(prepare(q)!=1)return -1;
    for(unsigned i=0;i<BT_SCO_LINKS;++i)if(q->links[i].phase==BT_SCO_UNUSED){
        struct bt_sco_link *l=&q->links[i];*l=(struct bt_sco_link){.ag=ag,.security=s,.generation=q->next_link++,
            .acl_generation=ag->link,.channel=ag->channel,.deadline=deadline,.acl_handle=p->handle,
            .handle=0xffff,.listening=listening,.codec=ag->selected_codec==2?2:1,.phase=BT_SCO_FEATURE_QUEUED};
        for(unsigned j=0;j<6;++j)l->address[j]=p->peer.address[j];*id=l->generation;return 1;
    }
    return 0;
}
int bt_sco_retry_cvsd(struct bt_sco *q,uint64_t old,uint64_t deadline,uint64_t *id)
{
    if(!valid(q)||!external(q,id,sizeof(*id))||deadline<=q->acl->hci->transport.now)return -1;
    struct bt_sco_link *l=link(q,old);struct bt_sco_security security={0};
    if(!l||l->codec!=2||!l->fallback_allowed||l->phase!=BT_SCO_CLOSED||l->handle_valid||
       l->ag->selected_codec!=1||!ag_current(q,l->ag,l->acl_generation,l->channel,&security)||
       security.generation!=l->security.generation||security.secure_connections!=l->security.secure_connections||
       q->next_link==UINT64_MAX)return -1;
    l->generation=q->next_link++;l->codec=1;l->fallback_allowed=0;l->profile=0;
    l->deadline=deadline;l->phase=BT_SCO_FEATURE_QUEUED;*id=l->generation;return 1;
}
static void parameters(const struct bt_sco_link *l,uint8_t *p)
{
    unsigned prefix=l->listening ? 6 : 2;
    if(l->listening)for(unsigned j=0;j<6;++j)p[j]=l->address[j];else put16(p,l->acl_handle);
    uint8_t *x=p+prefix;
    put32(x,8000);put32(x+4,8000);x[8]=2;x[13]=2;put16(x+18,60);put16(x+20,60);
    put32(x+22,16000);put32(x+26,16000);x[30]=4;x[35]=4;
    put16(x+40,16);put16(x+42,16);x[44]=2;x[45]=2;
    /* Data paths and transport unit sizes are zero: HCI, not physical PCM. */
    if(l->codec==2){x[8]=x[13]=x[30]=x[35]=3;put32(x+22,8000);put32(x+26,8000);}
    const unsigned masks[]={0,0x3c8,0x380,0x3c4,0x3c1,0x3c8,0x380};
    const unsigned latency[]={0,7,12,5,4,8,13};
    put16(x+52,latency[l->profile]);put16(x+54,masks[l->profile]);x[56]=l->codec==2?2:l->profile==1 ? 1 : l->profile==2 ? 2 : 0;
}
int bt_sco_step(struct bt_sco *q,unsigned budget,uint64_t deadline)
{
    if(!valid(q) || !budget || budget>BT_H4_BUDGET || !deadline)return -1;
    if(prepare(q)!=1)return -1;
    for(unsigned i=0;i<BT_SCO_LINKS;++i)if(q->links[i].phase!=BT_SCO_UNUSED && q->links[i].phase!=BT_SCO_CLOSED &&
        q->links[i].phase!=BT_SCO_OPEN && deadline>q->links[i].deadline)deadline=q->links[i].deadline;
    if(bt_acl_step(q->acl,budget,deadline)!=1)return fault(q,BT_SCO_LOWER);
    /* A full copied RX pool leaves the exact lower record owned. Retry bounded
     * classified records once capacity returns, preserving serial order. */
    struct bt_h4 *h=&q->acl->hci->transport;
    const struct bt_h4_record *r=0;
    for(unsigned i=0;i<h->count;++i){const struct bt_h4_record *c=&h->records[i];
        if(c->owner==BT_H4_OWNED && c->type==3 && q->held_rx[i]==c->serial && c->serial<=q->acl->seen_record && (!r || c->serial<r->serial))r=c;}
    if(r && synchronous_event(q,r)<0)return -1;
    if(prepare(q)!=1)return -1;
    if(q->active_tx<BT_SCO_PACKETS || q->acl->active_tx<BT_ACL_TX_SLOTS ||
       q->acl->hci->command!=BT_HCI_IDLE || q->acl->command_link<BT_ACL_LINKS)return 1;
    /* A refusal past its response bound is left to the controller timeout. */
    if(q->refuse==1 && q->now>=q->refuse_deadline)q->refuse=0;
    if(q->command_link==BT_SCO_LINKS && q->refuse==1){
        uint8_t p[7];for(unsigned j=0;j<6;++j)p[j]=q->refuse_address[j];p[6]=0x0d;
        int result=bt_hci_synchronous_request(q->acl->hci,q,0x042a,p,7,after(q->now,q->binding.response_timeout_us));
        if(result<0)return fault(q,BT_SCO_LOWER);
        if(result==1){q->refuse=2;q->refuse_tag=q->acl->hci->command_tag;}
        return 1;
    }
    if(q->command_link==BT_SCO_LINKS && q->refuse!=2){
        for(unsigned i=0;i<BT_SCO_LINKS;++i){struct bt_sco_link *l=&q->links[i];
            uint8_t p[63]={0};unsigned op=0;size_t n=0;
            if(l->phase==BT_SCO_FEATURE_QUEUED){op=0x041b;n=2;put16(p,l->acl_handle);}
            else if(l->phase==BT_SCO_SETUP_QUEUED){op=l->listening ? 0x043e : 0x043d;n=l->listening ? 63 : 59;parameters(l,p);}
            else if(l->phase==BT_SCO_REJECT_QUEUED){op=0x042a;n=7;for(unsigned j=0;j<6;++j)p[j]=l->address[j];p[6]=l->reason;}
            else if(l->phase==BT_SCO_DISCONNECT_QUEUED){op=0x0406;n=3;put16(p,l->handle);p[2]=0x13;}
            if(!op)continue;
            int result=bt_hci_synchronous_request(q->acl->hci,q,(uint16_t)op,p,n,l->deadline);
            if(result<0)return fault(q,BT_SCO_LOWER);
            if(result==1){++l->phase;l->command_tag=q->acl->hci->command_tag;q->command_link=i;}
            return 1;
        }
    }
    if(!q->credits)return 1;
    struct bt_sco_packet *t=0;unsigned index=0;
    for(unsigned i=0;i<BT_SCO_PACKETS;++i)if(q->tx[i].state==BT_SCO_QUEUED && (!t || q->tx[i].serial<t->serial)){t=&q->tx[i];index=i;}
    if(!t)return 1;struct bt_sco_link *l=link(q,t->link);
    if(!l || l->phase!=BT_SCO_OPEN){t->state=BT_SCO_NOT_SENT;return 1;}
    int accepted=bt_h4_sco(h,l->handle,t->bytes,t->length,t->deadline);
    if(accepted<0)return fault(q,BT_SCO_LOWER);
    if(accepted==1){--q->credits;t->state=BT_SCO_PARTIAL;q->active_tx=index;}return 1;
}
int bt_sco_send(struct bt_sco *q,uint64_t generation,const uint8_t *bytes,size_t n,uint64_t deadline,uint64_t *serial)
{
    if(!valid(q) || !n || n>255 || n>q->mtu || deadline<=q->acl->hci->transport.now ||
       !external(q,bytes,n) || !external(q,serial,sizeof(*serial)) || !bt_range_disjoint(bytes,n,serial,sizeof(*serial)))return -1;
    struct bt_sco_link *l=link(q,generation);if(!l || l->phase!=BT_SCO_OPEN || (l->codec!=2&&((n&1)||n>254)))return -1;
    if(prepare(q)!=1)return -1;
    if(q->next_packet==UINT64_MAX)return fault(q,BT_SCO_IDENTITY);
    for(unsigned i=0;i<BT_SCO_PACKETS;++i)if(q->tx[i].state==BT_SCO_FREE){struct bt_sco_packet *t=&q->tx[i];
        *t=(struct bt_sco_packet){.serial=q->next_packet++,.link=generation,.deadline=deadline,.length=n,.state=BT_SCO_QUEUED};
        for(size_t j=0;j<n;++j)t->bytes[j]=bytes[j];*serial=t->serial;return 1;}
    return 0;
}
static const struct bt_sco_packet *packet(const struct bt_sco *q,unsigned receive)
{
    if(!q || q->self!=q || !q->initialized)return 0;
    const struct bt_sco_packet *oldest=0,*pool=receive ? q->rx : q->tx;
    for(unsigned i=0;i<BT_SCO_PACKETS;++i)if((receive ? pool[i].state==BT_SCO_RECEIVED :
        pool[i].state>=BT_SCO_RETIRED && pool[i].state<=BT_SCO_NOT_SENT) && (!oldest || pool[i].serial<oldest->serial))oldest=&pool[i];
    return oldest;
}
const struct bt_sco_packet *bt_sco_result(const struct bt_sco *q){return packet(q,0);}
const struct bt_sco_packet *bt_sco_receive(const struct bt_sco *q){return packet(q,1);}
static int release_packet(struct bt_sco *q,uint64_t serial,unsigned receive)
{
    if(!valid(q) || !serial)return -1;struct bt_sco_packet *pool=receive ? q->rx : q->tx;
    for(unsigned i=0;i<BT_SCO_PACKETS;++i)if(pool[i].serial==serial &&
        (receive ? pool[i].state==BT_SCO_RECEIVED : pool[i].state>=BT_SCO_RETIRED && pool[i].state<=BT_SCO_NOT_SENT)){
        pool[i].state=BT_SCO_FREE;return 1;}
    return -1;
}
int bt_sco_release_result(struct bt_sco *q,uint64_t serial){return release_packet(q,serial,0);}
int bt_sco_release_receive(struct bt_sco *q,uint64_t serial){return release_packet(q,serial,1);}
int bt_sco_close(struct bt_sco *q,uint64_t generation,uint64_t deadline)
{
    if(!valid(q) || deadline<=q->acl->hci->transport.now)return -1;
    struct bt_sco_link *l=link(q,generation);if(!l || l->phase!=BT_SCO_OPEN)return -1;
    if(prepare(q)!=1)return -1;l->deadline=deadline;l->phase=BT_SCO_DISCONNECT_QUEUED;return 1;
}
