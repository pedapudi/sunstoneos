/* SPDX-License-Identifier: Apache-2.0 */
/* Original implementation of Bluetooth Core HCI packet flow control and
 * BR/EDR link commands. Reference structures remain external. */
#include "bt_acl.h"
static uint16_t le16(const uint8_t *p) { return (uint16_t)p[0]|((uint16_t)p[1]<<8); }
static int span(const void *p,size_t n) { return p && n && (uintptr_t)p<=UINTPTR_MAX-(n-1); }
static int overlap(const void *a,size_t an,const void *b,size_t bn)
{ return an && bn && ((uintptr_t)a<=(uintptr_t)b ? (uintptr_t)b-(uintptr_t)a<an : (uintptr_t)a-(uintptr_t)b<bn); }
static int private(const struct bt_acl *a,const void *p,size_t n)
{
    if(!span(p,n) || overlap(a,sizeof(*a),p,n) || overlap(a->hci,sizeof(*a->hci),p,n) ||
       overlap(a->hci->transport.binding.uart,sizeof(struct a733_uart),p,n)) return 0;
    for(unsigned i=0;i<a->hci->transport.count;++i)
        if(overlap(p,n,a->hci->transport.records[i].storage.bytes,
                   a->hci->transport.records[i].storage.capacity)) return 0;
    for(unsigned i=0;i<BT_ACL_TX_SLOTS;++i) if(a->packets[i].state!=BT_ACL_TX_FREE &&
       overlap(p,n,a->packets[i].bytes,a->packets[i].length)) return 0;
    return 1;
}
static int fault(struct bt_acl *a)
{ a->state=BT_ACL_FAULT;bt_h4_fail(&a->hci->transport);a->hci->state=BT_HCI_FAULT;return -1; }
static struct bt_acl_link *link(struct bt_acl *a,uint64_t generation)
{
    for(unsigned i=0;i<BT_ACL_LINKS;++i)
        if(generation && a->links[i].generation==generation) return &a->links[i];
    return 0;
}
static struct bt_acl_link *handle(struct bt_acl *a,unsigned value)
{
    for(unsigned i=0;i<BT_ACL_LINKS;++i)
        if(a->links[i].handle_valid && a->links[i].handle==value) return &a->links[i];
    return 0;
}
static int equal(const uint8_t *a,const uint8_t *b)
{ for(unsigned i=0;i<6;++i) if(a[i]!=b[i]) return 0;return 1; }
static int live(enum bt_acl_link_state s)
{ return s>=BT_ACL_CONNECTED && s<=BT_ACL_DISCONNECTING; }
static int pending(enum bt_acl_link_state s)
{ return s!=BT_ACL_UNUSED && s!=BT_ACL_CONNECTED && s!=BT_ACL_CLOSED && s!=BT_ACL_WAIT_REQUEST; }
int bt_acl_init(struct bt_acl *a,struct bt_hci *h)
{
    if(!a || !h || h->self!=h || h->acl_owner || a->state!=BT_ACL_OFF ||
       h->state!=BT_HCI_IDENTIFIED || h->identity.valid!=63 || h->external_command) return -1;
    unsigned configured=bt_hci_configuration_complete(h);
    if((!configured && (h->configured || h->command_tag!=6 || h->configuration_deadline)) ||
       h->command!=BT_HCI_IDLE || h->transport.state!=BT_H4_ACTIVE ||
       (!configured && (h->transport.staged || h->transport.assembling!=h->transport.count)) ||
       h->transport.tx_offset!=h->transport.tx_length ||
       overlap(a,sizeof(*a),h,sizeof(*h)) ||
       overlap(a,sizeof(*a),h->transport.binding.uart,sizeof(struct a733_uart)) ||
       !le16(h->identity.buffer_size) || !le16(h->identity.buffer_size+3) ||
       (h->identity.features[4]&0x20u) || (h->identity.supported_commands[0]&0x30u)!=0x30u) return -1;
    for(unsigned i=0;i<h->transport.count;++i)
        if((!configured && h->transport.records[i].owner!=BT_H4_FREE) ||
           overlap(a,sizeof(*a),h->transport.records[i].storage.bytes,h->transport.records[i].storage.capacity)) return -1;
    if(configured && bt_hci_incoming_bound(h,1)!=1) return -1;
    /* One serialized owner: after every admission check, neither assignment
     * calls external code or can fail. The claimed address cannot be reused. */
    h->acl_owner=a;
    *a=(struct bt_acl){.hci=h,.next_link=1,.next_packet=1,
        .capacity=le16(h->identity.buffer_size+3),.credits=le16(h->identity.buffer_size+3),
        .mtu=le16(h->identity.buffer_size),.active_tx=BT_ACL_TX_SLOTS,
        .command_link=BT_ACL_LINKS,.state=BT_ACL_ACTIVE};return 1;
}
int bt_acl_synchronous_register(struct bt_acl *a,const struct bt_acl_synchronous_ops *op,void *ctx)
{
    if(!a || a->state!=BT_ACL_ACTIVE || !op || !ctx || a->synchronous || a->hci->synchronous_owner ||
       !a->hci->synchronous_ready || !op->max_handles || op->max_handles>4 ||
       !op->prepare || !op->acknowledgement_pending || !op->owns || !op->validate_credits || !op->commit_credits ||
       !op->validate_disconnect || !op->commit_disconnect || !op->event ||
       !private(a,op,sizeof(*op)))return -1;
    a->synchronous=op;a->synchronous_context=ctx;a->hci->synchronous_owner=ctx;return 1;
}
int bt_acl_connect(struct bt_acl *a,const struct bt_acl_peer *p,uint64_t deadline,uint64_t *generation)
{
    if(!a || !a->hci || a->hci->self!=a->hci || a->hci->acl_owner!=a || a->state!=BT_ACL_ACTIVE || !p || !generation || !private(a,p,sizeof(*p)) ||
       !private(a,generation,sizeof(*generation)) || overlap(p,sizeof(*p),generation,sizeof(*generation)) ||
       deadline<=a->hci->transport.now || p->page_scan_mode>2 || p->role_switch>1) return -1;
    unsigned any=0;for(unsigned i=0;i<6;++i) any|=p->address[i];
    if(!any) return -1;
    for(unsigned i=0;i<BT_ACL_LINKS;++i)
        if(a->links[i].state!=BT_ACL_UNUSED &&
           equal(p->address,a->links[i].peer.address)) return -1;
    /* Serialize terminal link operations, beyond the HCI command credit slot. */
    for(unsigned i=0;i<BT_ACL_LINKS;++i) if(pending(a->links[i].state)) return 0;
    for(unsigned i=0;i<BT_ACL_LINKS;++i) if(a->links[i].state==BT_ACL_UNUSED) {
        a->links[i]=(struct bt_acl_link){.peer=*p,.generation=a->next_link++,
            .deadline=deadline,.handle=0xffff,.state=BT_ACL_CONNECT_QUEUED};
        *generation=a->links[i].generation;return 1;
    }
    return 0;
}
int bt_acl_incoming(struct bt_acl *a,uint64_t timeout)
{
    if(!a || !a->hci || a->hci->self!=a->hci || a->hci->acl_owner!=a || a->state!=BT_ACL_ACTIVE || !timeout || a->incoming_timeout_us ||
       a->next_link!=1 || a->hci->command!=BT_HCI_IDLE ||
       (a->hci->identity.supported_commands[1]&3u)!=3u ||
       timeout>UINT64_MAX-a->hci->transport.now) return -1;
    int bound=bt_hci_incoming_bound(a->hci,timeout);
    if(bound<0) return fault(a);
    if(bound!=1) return -1;
    a->incoming_timeout_us=timeout;return 1;
}
int bt_acl_listen(struct bt_acl *a,const uint8_t address[6],uint64_t deadline,uint64_t *generation)
{
    if(!a || !a->hci || a->hci->self!=a->hci || a->hci->acl_owner!=a || a->state!=BT_ACL_ACTIVE || !a->incoming_timeout_us ||
       !private(a,address,6) || !private(a,generation,sizeof(*generation)) ||
       overlap(address,6,generation,sizeof(*generation)) || deadline<=a->hci->transport.now ||
       equal(address,a->hci->identity.address)) return -1;
    unsigned any=0;for(unsigned i=0;i<6;++i) any|=address[i];
    if(!any) return -1;
    for(unsigned i=0;i<BT_ACL_LINKS;++i)
        if(a->links[i].state!=BT_ACL_UNUSED && equal(address,a->links[i].peer.address)) return -1;
    for(unsigned i=0;i<BT_ACL_LINKS;++i) if(a->links[i].state==BT_ACL_UNUSED) {
        struct bt_acl_link *l=&a->links[i];
        *l=(struct bt_acl_link){.generation=a->next_link++,.deadline=deadline,
            .handle=0xffff,.incoming=1,.state=BT_ACL_WAIT_REQUEST};
        for(unsigned j=0;j<6;++j) l->peer.address[j]=address[j];
        *generation=l->generation;return 1;
    }
    return 0;
}
int bt_acl_disconnect(struct bt_acl *a,uint64_t generation,uint64_t deadline)
{
    if(!a || !a->hci || a->hci->self!=a->hci || a->hci->acl_owner!=a || a->state!=BT_ACL_ACTIVE || deadline<=a->hci->transport.now) return -1;
    struct bt_acl_link *l=link(a,generation);
    if(!l || l->state!=BT_ACL_CONNECTED) return -1;
    for(unsigned i=0;i<BT_ACL_LINKS;++i) if(pending(a->links[i].state)) return 0;
    l->state=BT_ACL_DISCONNECT_QUEUED;l->deadline=deadline;l->status=0;l->reason=0;
    return 1;
}
int bt_acl_submit(struct bt_acl *a,uint64_t generation,unsigned pb,const uint8_t *bytes,
                  size_t n,uint64_t deadline,uint64_t *serial)
{
    if(!a || !a->hci || a->hci->self!=a->hci || a->hci->acl_owner!=a || a->state!=BT_ACL_ACTIVE || !serial || (pb!=1 && pb!=2) ||
       !n || n>a->mtu || deadline<=a->hci->transport.now ||
       !private(a,bytes,n) || !private(a,serial,sizeof(*serial)) || overlap(bytes,n,serial,sizeof(*serial))) return -1;
    struct bt_acl_link *l=link(a,generation);
    if(!l || l->state!=BT_ACL_CONNECTED || l->flush_seen) return -1;
    if(a->next_packet==UINT64_MAX) return fault(a);
    for(unsigned i=0;i<BT_ACL_TX_SLOTS;++i) if(a->packets[i].state==BT_ACL_TX_FREE) {
        a->packets[i]=(struct bt_acl_packet){.bytes=bytes,.length=n,.serial=a->next_packet++,
            .link_generation=generation,.deadline=deadline,.handle_flags=(uint16_t)(l->handle|(pb<<12)),
            .state=BT_ACL_QUEUED};*serial=a->packets[i].serial;return 1;
    }
    return 0;
}
static int release_record(struct bt_acl *a,const struct bt_h4_record *r)
{ return bt_hci_release(a->hci,r->serial)==1 ? 1 : fault(a); }
static int credit_event(struct bt_acl *a,const struct bt_h4_record *r)
{
    const uint8_t *p=r->storage.bytes;unsigned n=p[1],count=p[2],total=0;
    if(n!=1+4*count || count>BT_ACL_LINKS+(a->synchronous ? a->synchronous->max_handles : 0)) return fault(a);
    /* Validate every handle/count, including duplicates, before changing any
     * packet or credit. Retired handles cannot identify a new generation. */
    for(unsigned i=0;i<count;++i) {
        unsigned h=le16(p+3+4*i),c=le16(p+5+4*i),owned=0;
        struct bt_acl_link *l=handle(a,h);
        if(h>0xeff) return fault(a);
        for(unsigned j=0;j<i;++j) if(le16(p+3+4*j)==h) return fault(a);
        if(!l){if(!a->synchronous || a->synchronous->owns(a->synchronous_context,(uint16_t)h)!=1)return fault(a);continue;}
        if(!live(l->state))return fault(a);
        for(unsigned j=0;j<BT_ACL_TX_SLOTS;++j)
            if(a->packets[j].state==BT_ACL_SENT && a->packets[j].link_generation==l->generation) ++owned;
        if(c>owned) return fault(a);
        total+=c;
    }
    if(total>a->capacity-a->credits || (a->synchronous &&
       a->synchronous->validate_credits(a->synchronous_context,p+3,count)!=1)) return fault(a);
    for(unsigned i=0;i<count;++i) {
        struct bt_acl_link *l=handle(a,le16(p+3+4*i));
        if(!l)continue;
        for(unsigned c=le16(p+5+4*i);c;--c) {
            struct bt_acl_packet *oldest=0;
            for(unsigned j=0;j<BT_ACL_TX_SLOTS;++j) {
                struct bt_acl_packet *t=&a->packets[j];
                if(t->state==BT_ACL_SENT && t->link_generation==l->generation &&
                   (!oldest || t->serial<oldest->serial)) oldest=t;
            }
            oldest->state=BT_ACL_CREDIT_RETIRED;
        }
    }
    a->credits+=total;
    if(a->synchronous)a->synchronous->commit_credits(a->synchronous_context,p+3,count);
    return release_record(a,r);
}
static int incoming_request(struct bt_acl *a,const struct bt_h4_record *r)
{
    const uint8_t *p=r->storage.bytes;
    if(p[1]!=10 || p[11]>2) return fault(a);
    /* Synchronous requests belong to their own protocol owner. */
    if(p[11]!=1 || !a->incoming_timeout_us) return 1;
    unsigned any=0;for(unsigned j=0;j<6;++j) any|=p[2+j];
    if(!any || a->hci->transport.now>UINT64_MAX-a->incoming_timeout_us) return fault(a);
    uint64_t deadline=a->hci->transport.now+a->incoming_timeout_us;
    struct bt_acl_link *l=0,*unused=0;
    for(unsigned i=0;i<BT_ACL_LINKS;++i) {
        struct bt_acl_link *candidate=&a->links[i];
        if(candidate->state==BT_ACL_UNUSED) {if(!unused) unused=candidate;}
        else if(equal(candidate->peer.address,p+2)) l=candidate;
    }
    if(l) {
        if(l->state!=BT_ACL_WAIT_REQUEST || l->deadline<=a->hci->transport.now) return fault(a);
        if(l->deadline>deadline) l->deadline=deadline;
        l->state=BT_ACL_ACCEPT_QUEUED;
    } else {
        if(!unused) return fault(a);
        l=unused;
        *l=(struct bt_acl_link){.generation=a->next_link++,.deadline=deadline,
            .handle=0xffff,.incoming=1,.reason=0x0e,.state=BT_ACL_REJECT_QUEUED};
        if(equal(p+2,a->hci->identity.address)) l->reason=0x0f;
        for(unsigned j=0;j<6;++j) l->peer.address[j]=p[2+j];
    }
    for(unsigned j=0;j<3;++j) l->class_of_device[j]=p[8+j];
    l->request_serial=r->serial;
    return release_record(a,r);
}
static int dispatch(struct bt_acl *a,const struct bt_h4_record *r)
{
    if(a->synchronous && !(r->type==4 && (r->storage.bytes[0]==0x13 || r->storage.bytes[0]==0x05))){
        int owned=a->synchronous->event(a->synchronous_context,r);
        if(owned)return owned==1 ? 1 : fault(a);
    }
    if(r->type!=4) return 1;
    const uint8_t *p=r->storage.bytes;unsigned event=p[0],n=p[1];
    if(event==0x04) return incoming_request(a,r);
    if(event==0x13) { if(n<1) return fault(a);return credit_event(a,r); }
    if(event==0x0f || event==0x0e) {
        if(a->command_link==BT_ACL_LINKS) return 1;
        struct bt_acl_link *l=&a->links[a->command_link];
        unsigned op=le16(p+(event==0x0f ? 4 : 3));
        unsigned expected=l->state==BT_ACL_CONNECT_STATUS ? 0x0405 :
            l->state==BT_ACL_ACCEPT_STATUS ? 0x0409 :
            l->state==BT_ACL_REJECT_STATUS ? 0x040a : 0x0406;
        if(op!=expected || r->tag!=l->command_tag) return 1;
        if(a->hci->command!=BT_HCI_IDLE) return fault(a);
        l->status=p[event==0x0f ? 2 : 5];l->acknowledgement=r->serial;
        if(l->state==BT_ACL_CONNECT_STATUS)
            l->state=l->status ? BT_ACL_CLOSED : BT_ACL_CONNECTING;
        else if(l->state==BT_ACL_ACCEPT_STATUS)
            l->state=l->status ? BT_ACL_CLOSED : BT_ACL_ACCEPTING;
        else if(l->state==BT_ACL_REJECT_STATUS)
            l->state=l->status ? BT_ACL_CLOSED : BT_ACL_REJECTING;
        else if(l->state==BT_ACL_DISCONNECT_STATUS)
            l->state=l->status ? BT_ACL_CONNECTED : BT_ACL_DISCONNECTING;
        else return fault(a);
        a->command_link=BT_ACL_LINKS;return release_record(a,r);
    }
    if(event==0x03) {
        if(n!=11) return fault(a);
        struct bt_acl_link *l=0;
        for(unsigned i=0;i<BT_ACL_LINKS;++i) if(equal(a->links[i].peer.address,p+5) &&
            (a->links[i].state==BT_ACL_CONNECTING || a->links[i].state==BT_ACL_CONNECT_STATUS ||
             a->links[i].state==BT_ACL_ACCEPTING || a->links[i].state==BT_ACL_ACCEPT_STATUS ||
             a->links[i].state==BT_ACL_REJECTING || a->links[i].state==BT_ACL_REJECT_STATUS)) l=&a->links[i];
        if(!l) return p[2] ? 1 : fault(a);
        if((l->state!=BT_ACL_CONNECTING && l->state!=BT_ACL_ACCEPTING &&
            l->state!=BT_ACL_REJECTING) || r->serial<=l->acknowledgement) return fault(a);
        if(l->state==BT_ACL_REJECTING && !p[2]) return fault(a);
        if(p[2]) {l->status=p[2];l->state=BT_ACL_CLOSED;return release_record(a,r);}
        unsigned h=le16(p+3);
        if(h>0xeff || handle(a,h) || (a->synchronous && a->synchronous->owns(a->synchronous_context,(uint16_t)h)) || p[11]!=1 || p[12]>1) return fault(a);
        l->handle=(uint16_t)h;l->handle_valid=1;l->encryption=p[12];l->state=BT_ACL_CONNECTED;
        return release_record(a,r);
    }
    if(event==0x05 || event==0x11) {
        if(n!=(event==0x05 ? 4u : 2u)) return fault(a);
        unsigned h=le16(p+(event==0x05 ? 3 : 2));
        struct bt_acl_link *l=handle(a,h);
        if(h>0xeff)return fault(a);
        if(!l && event==0x05 && a->synchronous && a->synchronous->owns(a->synchronous_context,(uint16_t)h)==1){
            if(a->synchronous->validate_disconnect(a->synchronous_context,r)!=1)return fault(a);
            a->synchronous->commit_disconnect(a->synchronous_context,r);return release_record(a,r);
        }
        if(!l || !live(l->state))return fault(a);
        if(event==0x05 && p[2]) {
            if(l->state!=BT_ACL_DISCONNECTING) return fault(a);
            l->status=p[2];l->state=BT_ACL_CONNECTED;return release_record(a,r);
        }
        /* A remote disconnect can precede our local command acknowledgement.
         * Keep that operation uncertain instead of reusing its command slot. */
        if(l->state==BT_ACL_DISCONNECT_STATUS) return fault(a);
        for(unsigned i=0;i<BT_ACL_TX_SLOTS;++i)
            if(a->packets[i].link_generation==l->generation && a->packets[i].state==BT_ACL_PARTIAL)
                return fault(a);
        for(unsigned i=0;i<BT_ACL_TX_SLOTS;++i) {
            struct bt_acl_packet *t=&a->packets[i];
            if(t->link_generation!=l->generation) continue;
            if(t->state==BT_ACL_QUEUED) t->state=BT_ACL_NOT_SENT;
            if(t->state==BT_ACL_SENT) {
                t->uncertain=1;
                if(event==0x05) {t->state=BT_ACL_FLUSHED;++a->credits;}
            }
        }
        if(event==0x05) {l->state=BT_ACL_CLOSED;l->reason=p[5];}
        else l->flush_seen=1;
        return release_record(a,r);
    }
    return 1;
}
int bt_acl_step(struct bt_acl *a,unsigned budget,uint64_t deadline)
{
    if(!a || !a->hci || a->hci->self!=a->hci || a->hci->acl_owner!=a || a->state!=BT_ACL_ACTIVE || !deadline || !budget || budget>BT_H4_BUDGET) return -1;
    for(unsigned i=0;i<BT_ACL_LINKS;++i) if((pending(a->links[i].state) ||
        a->links[i].state==BT_ACL_WAIT_REQUEST) && deadline>a->links[i].deadline)
        deadline=a->links[i].deadline;
    for(unsigned i=0;i<BT_ACL_TX_SLOTS;++i) if(a->packets[i].state>=BT_ACL_QUEUED &&
        a->packets[i].state<=BT_ACL_SENT && deadline>a->packets[i].deadline) deadline=a->packets[i].deadline;
    if(a->synchronous && a->synchronous->prepare(a->synchronous_context)!=1)return fault(a);
    if(bt_hci_step(a->hci,budget,deadline)!=1) return fault(a);
    if(a->synchronous && a->synchronous->prepare(a->synchronous_context)!=1)return fault(a);
    if(a->active_tx<BT_ACL_TX_SLOTS && a->hci->transport.tx_offset==a->hci->transport.tx_length) {
        a->packets[a->active_tx].state=BT_ACL_SENT;a->active_tx=BT_ACL_TX_SLOTS;
    }
    const struct bt_h4_record *r=0;
    for(unsigned i=0;i<a->hci->transport.count;++i) {
        const struct bt_h4_record *candidate=&a->hci->transport.records[i];
        if(candidate->owner==BT_H4_OWNED && candidate->serial>a->seen_record &&
           (!r || candidate->serial<r->serial)) r=candidate;
    }
    if(r) {a->seen_record=r->serial;if(dispatch(a,r)!=1) return -1;}
    if(a->active_tx<BT_ACL_TX_SLOTS || a->hci->command!=BT_HCI_IDLE ||
       a->command_link<BT_ACL_LINKS || (a->synchronous &&
       a->synchronous->acknowledgement_pending(a->synchronous_context))) return 1;
    for(unsigned i=0;i<BT_ACL_LINKS;++i) {
        struct bt_acl_link *l=&a->links[i];
        if(l->state!=BT_ACL_CONNECT_QUEUED && l->state!=BT_ACL_DISCONNECT_QUEUED &&
           l->state!=BT_ACL_ACCEPT_QUEUED && l->state!=BT_ACL_REJECT_QUEUED) continue;
        uint8_t params[13]={0};size_t n;unsigned opcode;
        if(l->state==BT_ACL_CONNECT_QUEUED) {
            for(unsigned j=0;j<6;++j) params[j]=l->peer.address[j];
            params[6]=0x06;params[7]=0x33;params[8]=l->peer.page_scan_mode;
            params[10]=(uint8_t)l->peer.clock_offset;params[11]=(uint8_t)(l->peer.clock_offset>>8);
            params[12]=l->peer.role_switch;n=13;opcode=0x0405;
        } else if(l->state==BT_ACL_ACCEPT_QUEUED || l->state==BT_ACL_REJECT_QUEUED) {
            for(unsigned j=0;j<6;++j) params[j]=l->peer.address[j];
            params[6]=l->state==BT_ACL_ACCEPT_QUEUED ? 1 : l->reason;
            n=7;opcode=l->state==BT_ACL_ACCEPT_QUEUED ? 0x0409 : 0x040a;
        } else {params[0]=(uint8_t)l->handle;params[1]=(uint8_t)(l->handle>>8);params[2]=0x13;n=3;opcode=0x0406;}
        int accepted=bt_hci_request(a->hci,(uint16_t)opcode,params,n,l->deadline);
        if(accepted<0) return fault(a);
        if(accepted==1) {l->state=opcode==0x0405 ? BT_ACL_CONNECT_STATUS :
            opcode==0x0409 ? BT_ACL_ACCEPT_STATUS : opcode==0x040a ? BT_ACL_REJECT_STATUS : BT_ACL_DISCONNECT_STATUS;
            l->command_tag=a->hci->command_tag;a->command_link=i;}
        return 1;
    }
    if(!a->credits) return 1;
    struct bt_acl_packet *t=0;unsigned index=0;
    for(unsigned i=0;i<BT_ACL_TX_SLOTS;++i) if(a->packets[i].state==BT_ACL_QUEUED &&
        (!t || a->packets[i].serial<t->serial)) {t=&a->packets[i];index=i;}
    if(!t) return 1;
    struct bt_acl_link *l=link(a,t->link_generation);
    if(l->state!=BT_ACL_CONNECTED || l->flush_seen) {t->state=BT_ACL_NOT_SENT;return 1;}
    int accepted=bt_h4_acl(&a->hci->transport,t->handle_flags,t->bytes,t->length,t->deadline);
    if(accepted<0) return fault(a);
    if(accepted==1) {--a->credits;t->state=BT_ACL_PARTIAL;a->active_tx=index;}
    return 1;
}
const struct bt_acl_packet *bt_acl_result(const struct bt_acl *a)
{
    if(!a || !a->hci || a->hci->self!=a->hci || a->hci->acl_owner!=a || a->state==BT_ACL_OFF) return 0;
    const struct bt_acl_packet *p=0;
    for(unsigned i=0;i<BT_ACL_TX_SLOTS;++i) if(a->packets[i].state>=BT_ACL_CREDIT_RETIRED &&
        (!p || a->packets[i].serial<p->serial)) p=&a->packets[i];
    return p;
}
int bt_acl_release(struct bt_acl *a,uint64_t serial)
{
    if(!a || !a->hci || a->hci->self!=a->hci || a->hci->acl_owner!=a || a->state!=BT_ACL_ACTIVE || !serial) return -1;
    for(unsigned i=0;i<BT_ACL_TX_SLOTS;++i) if(a->packets[i].serial==serial &&
        a->packets[i].state>=BT_ACL_CREDIT_RETIRED) {a->packets[i].state=BT_ACL_TX_FREE;return 1;}
    return -1;
}
const struct bt_h4_record *bt_acl_receive(const struct bt_acl *a)
{
    if(!a || !a->hci || a->hci->self!=a->hci || a->hci->acl_owner!=a || a->state==BT_ACL_OFF) return 0;
    const struct bt_h4_record *r=bt_hci_next(a->hci);
    return r && r->serial<=a->seen_record ? r : 0;
}
int bt_acl_receive_release(struct bt_acl *a,uint64_t serial)
{
    if(!a || !a->hci || a->hci->self!=a->hci || a->hci->acl_owner!=a || a->state!=BT_ACL_ACTIVE || !serial || serial>a->seen_record) return -1;
    return bt_hci_release(a->hci,serial);
}
