/* SPDX-License-Identifier: Apache-2.0 */
/* Original bounded SDP client based on Bluetooth Core Vol3B protocol rules. */
#include "bt_sdp.h"
static unsigned be16(const uint8_t *p) {return ((unsigned)p[0]<<8)|p[1];}
static uint32_t be32(const uint8_t *p)
{return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];}
static void put16(uint8_t *p,unsigned v) {p[0]=(uint8_t)(v>>8);p[1]=(uint8_t)v;}
static void copy(uint8_t *d,const uint8_t *s,size_t n) {for(size_t i=0;i<n;++i)d[i]=s[i];}
static int overlaps(const void *a,size_t an,const void *b,size_t bn)
{return an && bn && ((uintptr_t)a<=(uintptr_t)b ? (uintptr_t)b-(uintptr_t)a<an : (uintptr_t)a-(uintptr_t)b<bn);}
static int disjoint(const struct bt_sdp *q,const struct bt_l2cap *l)
{
    if(overlaps(q,sizeof(*q),l,sizeof(*l)) || overlaps(q,sizeof(*q),l->acl,sizeof(*l->acl)) ||
       overlaps(q,sizeof(*q),l->acl->hci,sizeof(*l->acl->hci)) ||
       overlaps(q,sizeof(*q),l->acl->hci->transport.binding.uart,sizeof(struct a733_uart)))return 0;
    for(unsigned i=0;i<l->acl->hci->transport.count;++i)
        if(overlaps(q,sizeof(*q),l->acl->hci->transport.records[i].storage.bytes,
                    l->acl->hci->transport.records[i].storage.capacity))return 0;
    return 1;
}
static const struct bt_l2cap_channel *channel(const struct bt_sdp *q)
{
    for(unsigned i=0;i<BT_L2CAP_CHANNELS;++i)if(q->l2cap->channels[i].generation==q->channel &&
        q->l2cap->channels[i].state!=BT_L2CAP_UNUSED)return &q->l2cap->channels[i];
    return 0;
}
static int fault(struct bt_sdp *q)
{q->phase=BT_SDP_FAULT;if(q->status==BT_SDP_PENDING)q->status=BT_SDP_LINK_LOST;return -1;}
struct element { unsigned type, data, length, end; };
static int element(const uint8_t *p,unsigned n,unsigned at,struct element *e)
{
    if(at>=n)return 0;
    unsigned tag=p[at++],type=tag>>3,size=tag&7,bytes=0;
    if(type>8 || (type==0 && size) || ((type==1 || type==2) && size>4) ||
       (type==3 && size!=1 && size!=2 && size!=4) || (type==5 && size) ||
       ((type==4 || type==6 || type==7 || type==8) && size<5))return 0;
    if(size<5)bytes=type==0 ? 0 : 1u<<size;
    else {
        unsigned width=1u<<(size-5);if(width>n-at)return 0;
        if(width==1)bytes=p[at];else if(width==2)bytes=be16(p+at);else bytes=be32(p+at);
        at+=width;
    }
    if(bytes>n-at)return 0;
    *e=(struct element){type,at,bytes,at+bytes};return 1;
}
static int tree(const uint8_t *p,unsigned n)
{
    unsigned ends[BT_SDP_DEPTH],depth=1,at=0,nodes=0;ends[0]=n;
    while(depth) {
        if(at==ends[depth-1]){--depth;continue;}
        if(++nodes>BT_SDP_ELEMENTS)return -1;
        struct element e;if(!element(p,ends[depth-1],at,&e))return 0;
        at=e.end;
        if(e.type==6 || e.type==7) {
            if(depth==BT_SDP_DEPTH)return -1;
            ends[depth++]=e.end;at=e.data;
        }
    }
    return 1;
}
static int uuid16(const uint8_t *p,const struct element *e,unsigned uuid)
{
    if(e->type!=3)return 0;
    if(e->length==2)return be16(p+e->data)==uuid;
    if(e->length==4)return be32(p+e->data)==uuid;
    static const uint8_t tail[]={0,0,0x10,0,0x80,0,0,0x80,0x5f,0x9b,0x34,0xfb};
    if(be32(p+e->data)!=uuid)return 0;
    for(unsigned i=0;i<sizeof(tail);++i)if(p[e->data+4+i]!=tail[i])return 0;
    return 1;
}
static int classes(const uint8_t *p,const struct element *value,unsigned wanted,unsigned *matches)
{
    if(value->type!=6 || !value->length)return 0;
    for(unsigned at=value->data;at<value->end;) {
        struct element e;if(!element(p,value->end,at,&e) || e.type!=3)return 0;
        if(uuid16(p,&e,wanted))*matches=1;at=e.end;
    }
    return 1;
}
static int protocol_stack(const uint8_t *p,const struct element *stack,struct bt_sdp_record *r)
{
    if(stack->type!=6)return 0;
    unsigned at=stack->data,position=0,l2cap=0,psm=0;
    while(at<stack->end) {
        struct element descriptor,uuid,param;
        if(!element(p,stack->end,at,&descriptor) || descriptor.type!=6 ||
           !element(p,descriptor.end,descriptor.data,&uuid) || uuid.type!=3)return 0;
        if(position==0 && uuid16(p,&uuid,0x0100)) {
            l2cap=1;
            if(uuid.end<descriptor.end) {
                if(!element(p,descriptor.end,uuid.end,&param) || param.type!=1 || param.length!=2 ||
                    param.end!=descriptor.end)return 0;
                psm=be16(p+param.data);
            }
        } else if(position==1 && l2cap && uuid16(p,&uuid,3)) {
            if(!element(p,descriptor.end,uuid.end,&param) || param.type!=1 || param.length!=1 ||
                param.end!=descriptor.end)return 0;
            if(r->endpoints==BT_SDP_ENDPOINTS)return -1;
            r->endpoint[r->endpoints++]=(struct bt_sdp_endpoint){(uint16_t)psm,p[param.data]};
        }
        at=descriptor.end;++position;
    }
    return 1;
}
static int protocols(const uint8_t *p,const struct element *value,struct bt_sdp_record *r)
{
    if(value->type==6)return protocol_stack(p,value,r);
    if(value->type!=7)return 0;
    for(unsigned at=value->data;at<value->end;) {
        struct element stack;if(!element(p,value->end,at,&stack))return 0;
        int result=protocol_stack(p,&stack,r);if(result!=1)return result;at=stack.end;
    }
    return 1;
}
static enum bt_sdp_status parse(struct bt_sdp *q)
{
    int valid=tree(q->bytes,q->length);if(valid!=1)return valid<0 ? BT_SDP_LIMIT : BT_SDP_INVALID;
    struct element outer;
    if(!element(q->bytes,q->length,0,&outer) || outer.type!=6 || outer.end!=q->length)return BT_SDP_INVALID;
    struct bt_sdp_record records[BT_SDP_RECORDS]={0};
    struct bt_sdp_attribute attributes[BT_SDP_ATTRIBUTES]={0};
    unsigned rc=0,ac=0;
    for(unsigned at=outer.data;at<outer.end;) {
        if(rc==BT_SDP_RECORDS)return BT_SDP_LIMIT;
        struct element list;if(!element(q->bytes,outer.end,at,&list) || list.type!=6)return BT_SDP_INVALID;
        struct bt_sdp_record *r=&records[rc];r->first_attribute=(uint16_t)ac;
        unsigned seen=0,previous=0;
        for(unsigned cursor=list.data;cursor<list.end;) {
            struct element id,value;
            if(!element(q->bytes,list.end,cursor,&id) || id.type!=1 || id.length!=2 ||
                !element(q->bytes,list.end,id.end,&value) || value.type==0)return BT_SDP_INVALID;
            unsigned attribute=be16(q->bytes+id.data);
            if(r->attributes && attribute<=previous)return BT_SDP_INVALID;previous=attribute;
            if(ac==BT_SDP_ATTRIBUTES)return BT_SDP_LIMIT;
            attributes[ac++]=(struct bt_sdp_attribute){(uint16_t)attribute,(uint16_t)id.end,(uint16_t)(value.end-id.end)};
            ++r->attributes;
            if(attribute==0) {
                if(value.type!=1 || value.length!=4)return BT_SDP_INVALID;
                r->handle=be32(q->bytes+value.data);if(r->handle && r->handle<0x10000)return BT_SDP_INVALID;seen|=1;
            } else if(attribute==1) {
                if(!classes(q->bytes,&value,q->service_uuid,&r->matches_class))return BT_SDP_INVALID;seen|=2;
            } else if(attribute==4) {
                int result=protocols(q->bytes,&value,r);
                if(result!=1)return result<0 ? BT_SDP_LIMIT : BT_SDP_INVALID;
            }
            cursor=value.end;
        }
        if(seen!=3)return BT_SDP_INVALID;
        for(unsigned i=0;i<rc;++i)if(records[i].handle==r->handle)return BT_SDP_INVALID;
        ++rc;at=list.end;
    }
    for(unsigned i=0;i<rc;++i)q->record[i]=records[i];
    for(unsigned i=0;i<ac;++i)q->attribute[i]=attributes[i];
    q->records=rc;q->attributes=ac;return BT_SDP_COMPLETE;
}
int bt_sdp_begin(struct bt_sdp *q,struct bt_l2cap *l,uint64_t link,uint16_t uuid,uint64_t bound)
{
    if(!q || !l || q->phase!=BT_SDP_OFF || l->state!=BT_L2CAP_ACTIVE || !uuid ||
       bound<=l->acl->hci->transport.now || !disjoint(q,l))return -1;
    uint64_t identity;int result=bt_l2cap_open(l,link,1,bound,&identity);
    if(result!=1)return result;
    *q=(struct bt_sdp){.l2cap=l,.link=link,.channel=identity,.deadline=bound,.service_uuid=uuid,
        .transaction=1,.phase=BT_SDP_OPENING,.status=BT_SDP_PENDING};return 1;
}
static int submit(struct bt_sdp *q,const struct bt_l2cap_channel *c)
{
    uint8_t request[36]={6,0,0,0,0,0x35,3,0x19,0,0,0,0,0x35,5,0x0a,0,0,0xff,0xff,0};
    unsigned n=20+q->continuation_length;
    if(n>c->remote_mtu || c->local_mtu<31){q->status=BT_SDP_LIMIT;return 1;}
    /* Reserve the SDP header, byte count and largest continuation field. */
    q->maximum_bytes=(uint16_t)(c->local_mtu-24);
    put16(request+1,q->transaction);put16(request+3,n-5);put16(request+8,q->service_uuid);
    put16(request+10,q->maximum_bytes);request[19]=(uint8_t)q->continuation_length;
    copy(request+20,q->continuation,q->continuation_length);
    uint64_t now=q->l2cap->acl->hci->transport.now;
    if(now>UINT64_MAX-q->l2cap->ertx_us){q->status=BT_SDP_LIMIT;return 1;}
    uint64_t serial;int result=bt_l2cap_send(q->l2cap,q->channel,request,n,now+q->l2cap->ertx_us,&serial);
    if(result<0)return fault(q);
    if(result==1){q->tx_serial=serial;q->waiting=1;q->sent=0;q->request_after=0;}
    return 1;
}
static int transmit_result(struct bt_sdp *q)
{
    if(!q->tx_serial)return 1;
    for(unsigned i=0;i<BT_L2CAP_PDUS;++i) {
        const struct bt_l2cap_pdu *p=&q->l2cap->tx[i];
        if(p->state==BT_L2CAP_FREE || p->serial!=q->tx_serial)continue;
        if(p->channel!=q->channel)return fault(q);
        if(!q->sent && p->accepted==p->length) {
            q->sent=1;q->request_after=q->l2cap->acl->hci->transport.next_serial-1;
        }
        if(p->state==BT_L2CAP_COMPLETED || p->state==BT_L2CAP_FAILED) {
            if(p->state==BT_L2CAP_FAILED && q->status==BT_SDP_PENDING)q->status=BT_SDP_LINK_LOST;
            if(bt_l2cap_release_result(q->l2cap,p->serial)!=1)return fault(q);q->tx_serial=0;
        }
        return 1;
    }
    return fault(q);
}
static void response(struct bt_sdp *q,const struct bt_l2cap_pdu *p)
{
    const uint8_t *data=p->bytes+4;size_t n=p->length-4;
    if(n<5){q->status=BT_SDP_INVALID;q->held_rx=p->serial;return;}
    if(be16(data+1)!=q->transaction || !q->waiting || !q->sent || p->first_record<=q->request_after)return;
    if(be16(data+3)!=n-5){q->status=BT_SDP_INVALID;q->held_rx=p->serial;return;}
    if(data[0]==1) {
        if(n!=7){q->status=BT_SDP_INVALID;q->held_rx=p->serial;return;}
        q->remote_error=(uint16_t)be16(data+5);q->status=BT_SDP_REMOTE_ERROR;q->waiting=0;return;
    }
    if(data[0]!=7 || n<10){q->status=BT_SDP_INVALID;q->held_rx=p->serial;return;}
    unsigned bytes=be16(data+5);
    if(bytes<2 || bytes>q->maximum_bytes || bytes>n-8){q->status=BT_SDP_INVALID;q->held_rx=p->serial;return;}
    unsigned cn=data[7+bytes];
    if(cn>16 || n!=8+bytes+cn){q->status=BT_SDP_INVALID;q->held_rx=p->serial;return;}
    if(bytes>BT_SDP_BYTES-q->length || q->segments==BT_SDP_SEGMENTS) {
        q->status=BT_SDP_LIMIT;q->held_rx=p->serial;return;
    }
    copy(q->bytes+q->length,data+7,bytes);q->length+=bytes;++q->segments;q->waiting=0;
    if(!cn){q->status=parse(q);if(q->status!=BT_SDP_COMPLETE)q->held_rx=p->serial;return;}
    if(q->segments==BT_SDP_SEGMENTS || q->transaction==0xffff) {
        q->status=BT_SDP_LIMIT;q->held_rx=p->serial;return;
    }
    /* The continuation is opaque; repeated bytes do not imply no progress. */
    copy(q->continuation,data+8+bytes,cn);q->continuation_length=cn;++q->transaction;
}
int bt_sdp_close(struct bt_sdp *q,uint64_t bound)
{
    if(!q || q->phase==BT_SDP_OFF || q->phase==BT_SDP_FAULT ||
       q->phase==BT_SDP_CLOSED || q->phase==BT_SDP_CLOSING ||
       bound<=q->l2cap->acl->hci->transport.now)return -1;
    if(q->status==BT_SDP_PENDING)q->status=BT_SDP_CANCELLED;
    q->phase=BT_SDP_CLOSING;q->close_deadline=bound;return 1;
}
static int closing(struct bt_sdp *q,const struct bt_l2cap_channel *c)
{
    uint64_t now=q->l2cap->acl->hci->transport.now;
    if(now>=q->close_deadline)return fault(q);
    if(c->state==BT_L2CAP_OPEN) {
        int result=bt_l2cap_close(q->l2cap,q->channel,q->close_deadline);
        if(result<0)return fault(q);
    } else if(c->state==BT_L2CAP_CLOSED && !q->tx_serial && !q->held_rx) {
        int result=bt_l2cap_release_channel(q->l2cap,q->channel);
        if(result<0)return fault(q);
        if(result==1){q->phase=BT_SDP_CLOSED;return 0;}
    }
    return 1;
}
int bt_sdp_step(struct bt_sdp *q,unsigned budget,uint64_t bound)
{
    if(!q || q->phase==BT_SDP_OFF || q->phase==BT_SDP_FAULT)return -1;
    if(q->phase==BT_SDP_CLOSED)return 0;
    if(bt_l2cap_step(q->l2cap,budget,bound)!=1)return fault(q);
    if(transmit_result(q)!=1)return -1;
    const struct bt_l2cap_channel *c=channel(q);if(!c || c->link!=q->link)return fault(q);
    uint64_t now=q->l2cap->acl->hci->transport.now;
    if(q->status==BT_SDP_PENDING) {
        if(now>=q->deadline)q->status=BT_SDP_EXPIRED;
        else if(c->state==BT_L2CAP_CLOSED || c->state==BT_L2CAP_QUARANTINED)q->status=BT_SDP_LINK_LOST;
    }
    if(q->phase==BT_SDP_OPENING && c->state==BT_L2CAP_OPEN)q->phase=BT_SDP_RUNNING;
    const struct bt_l2cap_pdu *rx=0;
    for(unsigned i=0;i<BT_L2CAP_PDUS;++i) {
        const struct bt_l2cap_pdu *p=&q->l2cap->rx[i];
        if(p->state==BT_L2CAP_RECEIVED && p->channel==q->channel && (!rx || p->serial<rx->serial))rx=p;
    }
    if(rx) {
        if(q->status==BT_SDP_PENDING && !q->held_rx)response(q,rx);
        if(q->phase==BT_SDP_CLOSING || q->held_rx!=rx->serial) {
            uint64_t serial=rx->serial;if(bt_l2cap_release_receive(q->l2cap,serial)!=1)return fault(q);
            if(q->held_rx==serial)q->held_rx=0;
        }
    }
    if(q->phase==BT_SDP_CLOSING)return closing(q,c);
    if(q->phase==BT_SDP_RUNNING && q->status==BT_SDP_PENDING && !q->waiting && !q->tx_serial)
        return submit(q,c);
    return 1;
}
const struct bt_sdp_record *bt_sdp_record(const struct bt_sdp *q,unsigned index)
{return q && q->status==BT_SDP_COMPLETE && index<q->records ? &q->record[index] : 0;}
const struct bt_sdp_attribute *bt_sdp_attribute(const struct bt_sdp *q,unsigned record,uint16_t id)
{
    const struct bt_sdp_record *r=bt_sdp_record(q,record);if(!r)return 0;
    for(unsigned i=0;i<r->attributes;++i) {
        const struct bt_sdp_attribute *a=&q->attribute[r->first_attribute+i];if(a->id==id)return a;
    }
    return 0;
}
int bt_sdp_release(struct bt_sdp *q)
{if(!q || q->phase!=BT_SDP_CLOSED)return -1;*q=(struct bt_sdp){0};return 1;}
