/* SPDX-License-Identifier: Apache-2.0 */
/* Original bounded implementation of Bluetooth Core BR/EDR L2CAP basic mode.
 * Published kernel wire structures are retained in external reference fixtures. */
#include "bt_l2cap.h"
static uint16_t get16(const uint8_t *p) {return (uint16_t)p[0]|((uint16_t)p[1]<<8);}
static void put16(uint8_t *p,unsigned n) {p[0]=(uint8_t)n;p[1]=(uint8_t)(n>>8);}
static void copy(uint8_t *d,const uint8_t *s,size_t n) {for(size_t i=0;i<n;++i)d[i]=s[i];}
static int equal(const uint8_t *a,const uint8_t *b,size_t n)
{for(size_t i=0;i<n;++i)if(a[i]!=b[i])return 0;return 1;}
static int overlaps(const void *a,size_t an,const void *b,size_t bn)
{return an && bn && ((uintptr_t)a<=(uintptr_t)b ? (uintptr_t)b-(uintptr_t)a<an : (uintptr_t)a-(uintptr_t)b<bn);}
static int outside(const struct bt_l2cap *q,const void *p,size_t n)
{
    if(!n)return 1;
    if(!p || (uintptr_t)p>UINTPTR_MAX-(n-1) || overlaps(q,sizeof(*q),p,n) ||
       overlaps(q->acl,sizeof(*q->acl),p,n) || overlaps(q->acl->hci,sizeof(*q->acl->hci),p,n) ||
       overlaps(q->acl->hci->transport.binding.uart,sizeof(struct a733_uart),p,n))return 0;
    for(unsigned i=0;i<q->acl->hci->transport.count;++i)
        if(overlaps(p,n,q->acl->hci->transport.records[i].storage.bytes,
                    q->acl->hci->transport.records[i].storage.capacity))return 0;
    return 1;
}
static int fault(struct bt_l2cap *q,enum bt_l2cap_error e)
{q->state=BT_L2CAP_FAULT;q->error=e;return e==BT_L2CAP_CAPACITY ? BT_L2CAP_EXHAUSTED : -1;}
static unsigned link_index(const struct bt_l2cap *q,uint64_t generation)
{
    for(unsigned i=0;i<BT_ACL_LINKS;++i)if(generation && q->acl->links[i].generation==generation)return i;
    return BT_ACL_LINKS;
}
static int live(const struct bt_l2cap *q,uint64_t generation)
{
    unsigned i=link_index(q,generation);
    return i<BT_ACL_LINKS && q->acl->links[i].state==BT_ACL_CONNECTED && !q->acl->links[i].flush_seen;
}
static struct bt_l2cap_channel *channel(struct bt_l2cap *q,unsigned cid,uint64_t link)
{
    for(unsigned i=0;i<BT_L2CAP_CHANNELS;++i)if(q->channels[i].state!=BT_L2CAP_UNUSED &&
       q->channels[i].local_cid==cid && (!link || q->channels[i].link==link))return &q->channels[i];
    return 0;
}
static struct bt_l2cap_pdu *pdu(struct bt_l2cap_pdu *pool,uint64_t serial)
{
    for(unsigned i=0;i<BT_L2CAP_PDUS;++i)if(pool[i].state!=BT_L2CAP_FREE && pool[i].serial==serial)return &pool[i];
    return 0;
}
static struct bt_l2cap_pdu *free_pdu(struct bt_l2cap_pdu *pool)
{for(unsigned i=0;i<BT_L2CAP_PDUS;++i)if(pool[i].state==BT_L2CAP_FREE)return &pool[i];return 0;}
static int deadline(struct bt_l2cap *q,uint64_t *d)
{
    uint64_t now=q->acl->hci->transport.now;
    if(now>UINT64_MAX-q->timeout_us)return fault(q,BT_L2CAP_TIMEOUT);
    *d=now+q->timeout_us;return 1;
}
static int emit(struct bt_l2cap *q,uint64_t link,unsigned cid,const uint8_t *bytes,size_t n,
                uint64_t bound,unsigned application,uint64_t *serial)
{
    struct bt_l2cap_pdu *p=free_pdu(q->tx);
    if(!p)return 0;
    if(q->next_serial==UINT64_MAX)return fault(q,BT_L2CAP_CAPACITY);
    *p=(struct bt_l2cap_pdu){.serial=q->next_serial++,.link=link,.deadline=bound,
        .length=n+4,.application=application,.cid=(uint16_t)cid,.state=BT_L2CAP_TRANSMITTING};
    put16(p->bytes,(unsigned)n);put16(p->bytes+2,cid);copy(p->bytes+4,bytes,n);
    if(serial)*serial=p->serial;return 1;
}
static int command(struct bt_l2cap *q,uint64_t link,unsigned code,unsigned id,
                   const uint8_t *bytes,unsigned n,uint64_t bound,uint64_t *serial)
{
    uint8_t cmd[BT_L2CAP_SIGNAL_MTU];
    if(n>sizeof(cmd)-4 || !id)return fault(q,BT_L2CAP_PROTOCOL);
    cmd[0]=(uint8_t)code;cmd[1]=(uint8_t)id;put16(cmd+2,n);copy(cmd+4,bytes,n);
    return emit(q,link,1,cmd,n+4,bound,0,serial);
}
static int request(struct bt_l2cap *q,struct bt_l2cap_channel *c,unsigned code,const uint8_t *p,unsigned n)
{
    unsigned li=link_index(q,c->link);
    if(li==BT_ACL_LINKS || q->next_id[li]>255)return BT_L2CAP_EXHAUSTED;
    uint64_t serial;unsigned id=q->next_id[li];
    int r=command(q,c->link,code,id,p,n,c->deadline,&serial);
    if(r==1) {++q->next_id[li];c->request_code=code;c->request_id=id;
        c->request_serial=serial;c->request_sent=0;c->request_after=0;}
    return r;
}
static int configure(struct bt_l2cap *q,struct bt_l2cap_channel *c,unsigned empty)
{
    uint8_t bytes[8]={0};put16(bytes,c->remote_cid);
    bytes[4]=1;bytes[5]=2;put16(bytes+6,c->local_mtu);
    int r=request(q,c,4,bytes,empty ? 4 : 8);
    if(r==1)c->need_config=0;return r;
}
static int disconnect(struct bt_l2cap *q,struct bt_l2cap_channel *c)
{
    uint8_t bytes[4];put16(bytes,c->remote_cid);put16(bytes+2,c->local_cid);
    int r=request(q,c,6,bytes,4);if(r==1)c->state=BT_L2CAP_DISCONNECTING;return r;
}
int bt_l2cap_init(struct bt_l2cap *q,struct bt_acl *a,uint16_t mtu,uint64_t timeout,
                  int (*authorize)(void *,uint64_t,uint16_t,unsigned),void *context)
{
    if(!q || !a || q->state!=BT_L2CAP_OFF || a->state!=BT_ACL_ACTIVE || !authorize ||
       mtu<48 || mtu>BT_L2CAP_MTU || timeout<1000000 || timeout>60000000 ||
       overlaps(q,sizeof(*q),a,sizeof(*a)) || overlaps(q,sizeof(*q),a->hci,sizeof(*a->hci)) ||
       overlaps(q,sizeof(*q),a->hci->transport.binding.uart,sizeof(struct a733_uart)))return -1;
    for(unsigned i=0;i<BT_ACL_TX_SLOTS;++i)if(a->packets[i].state!=BT_ACL_TX_FREE)return -1;
    for(unsigned i=0;i<a->hci->transport.count;++i) {
        const struct bt_h4_record *r=&a->hci->transport.records[i];
        if(overlaps(q,sizeof(*q),r->storage.bytes,r->storage.capacity) ||
           (r->owner!=BT_H4_FREE && r->type==2))return -1;
    }
    if(a->hci->transport.staged || a->hci->transport.assembling!=a->hci->transport.count ||
       a->hci->command!=BT_HCI_IDLE)return -1;
    for(unsigned i=0;i<BT_ACL_LINKS;++i)if(a->links[i].state!=BT_ACL_UNUSED &&
       a->links[i].state!=BT_ACL_CONNECTED && a->links[i].state!=BT_ACL_CLOSED)return -1;
    *q=(struct bt_l2cap){.acl=a,.timeout_us=timeout,.local_mtu=mtu,
        .authorize=authorize,.context=context,.next_serial=1,.state=BT_L2CAP_ACTIVE};
    for(unsigned i=0;i<BT_ACL_LINKS;++i){q->next_id[i]=1;q->assembling[i]=BT_L2CAP_PDUS;}
    return 1;
}
int bt_l2cap_open(struct bt_l2cap *q,uint64_t link,uint16_t psm,uint64_t bound,uint16_t *cid)
{
    if(!q || q->state!=BT_L2CAP_ACTIVE || !cid || !outside(q,cid,sizeof(*cid)) ||
       !live(q,link) || !(psm&1) || (psm&0x100) || bound<=q->acl->hci->transport.now)return -1;
    unsigned li=link_index(q,link),slot=BT_L2CAP_CHANNELS;
    if(q->next_id[li]>255)return BT_L2CAP_EXHAUSTED;
    for(unsigned i=0;i<BT_L2CAP_CHANNELS;++i)if(q->channels[i].state==BT_L2CAP_UNUSED){slot=i;break;}
    if(slot==BT_L2CAP_CHANNELS)return BT_L2CAP_EXHAUSTED;
    if(!free_pdu(q->tx))return 0;
    uint64_t limit;if(deadline(q,&limit)!=1)return -1;if(bound>limit)bound=limit;
    if(q->authorize(q->context,link,psm,0)!=1)return -1;
    struct bt_l2cap_channel c={.link=link,.deadline=bound,.local_cid=(uint16_t)(0x40+slot),
        .psm=psm,.local_mtu=q->local_mtu,.remote_mtu=672,.state=BT_L2CAP_CONNECTING};
    uint8_t bytes[4];put16(bytes,psm);put16(bytes+2,c.local_cid);
    int r=request(q,&c,2,bytes,4);
    if(r==1){q->channels[slot]=c;*cid=c.local_cid;}return r;
}
int bt_l2cap_close(struct bt_l2cap *q,uint16_t cid,uint64_t bound)
{
    if(!q || q->state!=BT_L2CAP_ACTIVE || bound<=q->acl->hci->transport.now)return -1;
    struct bt_l2cap_channel *c=channel(q,cid,0);
    if(!c || c->state!=BT_L2CAP_OPEN || !live(q,c->link))return -1;
    uint64_t limit;if(deadline(q,&limit)!=1)return -1;if(bound>limit)bound=limit;
    uint64_t old=c->deadline;c->deadline=bound;int r=disconnect(q,c);if(r!=1)c->deadline=old;return r;
}
int bt_l2cap_send(struct bt_l2cap *q,uint16_t cid,const uint8_t *p,size_t n,uint64_t bound,uint64_t *serial)
{
    if(!q || q->state!=BT_L2CAP_ACTIVE || !serial || !outside(q,serial,sizeof(*serial)) ||
       !outside(q,p,n) || overlaps(p,n,serial,sizeof(*serial)) || n>BT_L2CAP_MTU ||
       bound<=q->acl->hci->transport.now)return -1;
    struct bt_l2cap_channel *c=channel(q,cid,0);
    if(!c || c->state!=BT_L2CAP_OPEN || !live(q,c->link) || n>c->remote_mtu)return -1;
    return emit(q,c->link,c->remote_cid,p,n,bound,1,serial);
}
/* Parse complete TLVs before updating either direction's negotiated MTU.
 * Mandatory unknown options are echoed; hints are ignored. */
static int options(const uint8_t *p,unsigned n,unsigned *mtu,uint8_t *reply,unsigned *reply_n)
{
    unsigned result=0,seen=0,unacceptable=0,unknown=0;*reply_n=0;
    for(unsigned at=0;at<n;) {
        if(n-at<2 || p[at+1]>n-at-2)return -1;
        unsigned type=p[at]&0x7f,len=p[at+1],flag=type<8 ? 1u<<type : 0;
        if((flag && (seen&flag)) || ((type==1 || type==2) && len!=2) || (type==4 && len!=9))return -1;
        seen|=flag;at+=len+2;
    }
    seen=0;
    for(unsigned at=0;at<n;) {
        if(n-at<2 || p[at+1]>n-at-2)return -1;
        unsigned type=p[at]&0x7f,len=p[at+1],size=len+2;
        unsigned flag=type<8 ? 1u<<type : 0;
        if(flag && (seen&flag))return -1;seen|=flag;
        if(type==1) {
            if(len!=2)return -1;
            unsigned value=get16(p+at+2);
            if(value<48) {result=1;unacceptable=1;if(*reply_n+4>38){*reply_n=0;return 2;}
                reply[(*reply_n)++]=1;reply[(*reply_n)++]=2;put16(reply+*reply_n,48);*reply_n+=2;}
            else *mtu=value;
        } else if(type==4) {
            if(len!=9)return -1;
            if(p[at+2]) {result=1;unacceptable=1;if(*reply_n+11>38){*reply_n=0;return 2;}
                reply[(*reply_n)++]=4;reply[(*reply_n)++]=9;
                for(unsigned j=0;j<9;++j)reply[(*reply_n)++]=0;}
        } else if(type==2) {
            if(len!=2)return -1;
            if(get16(p+at+2)!=0xffff) {result=1;unacceptable=1;if(*reply_n+4>38){*reply_n=0;return 2;}
                reply[(*reply_n)++]=2;reply[(*reply_n)++]=2;put16(reply+*reply_n,0xffff);*reply_n+=2;}
        } else if(!(p[at]&0x80)) {
            if(*reply_n+size>38){*reply_n=0;return 2;}
            copy(reply+*reply_n,p+at,size);*reply_n+=size;result=3;unknown=1;
        }
        at+=size;
    }
    if(unknown && unacceptable){*reply_n=0;return 2;}
    return (int)result;
}
static void ready(struct bt_l2cap_channel *c)
{if(c->state==BT_L2CAP_CONFIGURING && c->input_done && c->output_done)c->state=BT_L2CAP_OPEN;}
static int response(struct bt_l2cap *q,struct bt_l2cap_pdu *rx,const uint8_t *cmd,unsigned n)
{
    unsigned code=cmd[0],id=cmd[1];const uint8_t *p=cmd+4;
    struct bt_l2cap_channel *c=0;
    for(unsigned i=0;i<BT_L2CAP_CHANNELS;++i)if(q->channels[i].link==rx->link &&
       q->channels[i].request_id==id && q->channels[i].request_sent &&
       rx->first_record>q->channels[i].request_after){c=&q->channels[i];break;}
    if(!c)return 1; /* Invalid, duplicate or late identifier: silently discard. */
    if(code==1) {if(n<2)return fault(q,BT_L2CAP_PROTOCOL);c->result=get16(p);
        if(c->request_code==6)return fault(q,BT_L2CAP_PROTOCOL);
        if(c->remote_cid) {int r=disconnect(q,c);return r<0 ? fault(q,BT_L2CAP_CAPACITY) : r;}
        c->state=BT_L2CAP_CLOSED;c->request_id=0;return 1;}
    if(code!=c->request_code+1)return 1;
    if(code==3) {
        if(n!=8 || get16(p+2)!=c->local_cid || c->state!=BT_L2CAP_CONNECTING)return fault(q,BT_L2CAP_PROTOCOL);
        unsigned result=get16(p+4);if(result==1)return 1;
        if(result){c->result=(uint16_t)result;c->state=BT_L2CAP_CLOSED;c->request_id=0;return 1;}
        unsigned remote=get16(p);if(remote<0x40 || get16(p+6))return fault(q,BT_L2CAP_PROTOCOL);
        for(unsigned i=0;i<BT_L2CAP_CHANNELS;++i)if(q->channels[i].state!=BT_L2CAP_UNUSED &&
           q->channels[i].link==c->link && q->channels[i].remote_cid==remote)return fault(q,BT_L2CAP_PROTOCOL);
        c->remote_cid=(uint16_t)remote;c->state=BT_L2CAP_CONFIGURING;c->request_id=0;c->need_config=1;return 1;
    }
    if(code==5) {
        if(n<6 || get16(p)!=c->local_cid || (get16(p+2)&~1u) || c->state!=BT_L2CAP_CONFIGURING)return fault(q,BT_L2CAP_PROTOCOL);
        unsigned result=get16(p+4),mtu=c->local_mtu,outn;uint8_t out[BT_L2CAP_SIGNAL_MTU];
        int parsed=options(p+6,n-6,&mtu,out,&outn);
        if(parsed<0 || (result==0 && parsed))return fault(q,BT_L2CAP_PROTOCOL);
        if(result==4)return 1;
        if(result==0 && mtu<=c->local_mtu && mtu>=48) {
            c->local_mtu=(uint16_t)mtu;
            if(get16(p+2)&1) {c->need_config=2;c->request_id=0;return 1;}
            c->input_done=1;c->request_id=0;ready(c);return 1;
        }
        c->result=(uint16_t)result;
        if(result==1 && mtu>=48 && mtu<=c->local_mtu && c->config_retries<2) {
            ++c->config_retries;c->local_mtu=(uint16_t)mtu;c->need_config=1;c->request_id=0;return 1;
        }
        int r=disconnect(q,c);return r<0 ? fault(q,BT_L2CAP_CAPACITY) : r;
    }
    if(code==7) {
        if(n!=4 || get16(p)!=c->remote_cid || get16(p+2)!=c->local_cid ||
           c->state!=BT_L2CAP_DISCONNECTING)return fault(q,BT_L2CAP_PROTOCOL);
        c->state=BT_L2CAP_CLOSED;c->request_id=0;return 1;
    }
    return 1;
}
static unsigned reject(uint8_t *reply,unsigned id,unsigned reason,unsigned local,unsigned remote)
{
    reply[0]=1;reply[1]=(uint8_t)id;unsigned n=reason==2 ? 6 : reason==1 ? 4 : 2;
    put16(reply+2,n);put16(reply+4,reason);
    if(reason==1)put16(reply+6,BT_L2CAP_SIGNAL_MTU);
    if(reason==2){put16(reply+6,local);put16(reply+8,remote);}return n+4;
}
static int peer_request(struct bt_l2cap *q,struct bt_l2cap_pdu *rx,const uint8_t *cmd,unsigned n)
{
    struct bt_l2cap_request *cache=0;
    for(unsigned i=0;i<BT_L2CAP_REQUESTS;++i) {
        struct bt_l2cap_request *v=&q->requests[i];
        if(v->length && v->link==rx->link && v->request[1]==cmd[1]) {
            if(v->length!=n+4 || !equal(v->request,cmd,n+4))return fault(q,BT_L2CAP_PROTOCOL);
            if(pdu(q->tx,v->reply_serial))return 1;
            uint64_t bound;if(deadline(q,&bound)!=1)return -1;
            return emit(q,rx->link,1,v->reply,v->reply_length,bound,0,&v->reply_serial);
        }
        if(!v->length && !cache)cache=v;
    }
    if(!cache)return fault(q,BT_L2CAP_CAPACITY);
    if(!free_pdu(q->tx))return 0;
    unsigned code=cmd[0],id=cmd[1],rn=0;const uint8_t *p=cmd+4;
    uint8_t reply[BT_L2CAP_SIGNAL_MTU]={0};reply[0]=(uint8_t)(code+1);reply[1]=(uint8_t)id;
    struct bt_l2cap_channel *c=0,updated={0};unsigned slot=BT_L2CAP_CHANNELS;
    uint64_t bound;if(deadline(q,&bound)!=1)return -1;
    if(code==2) {
        if(n!=4)return fault(q,BT_L2CAP_PROTOCOL);
        unsigned psm=get16(p),remote=get16(p+2),result=0;
        for(unsigned i=0;i<BT_L2CAP_CHANNELS;++i) {
            if(q->channels[i].state==BT_L2CAP_UNUSED && slot==BT_L2CAP_CHANNELS)slot=i;
            if(q->channels[i].state!=BT_L2CAP_UNUSED && q->channels[i].link==rx->link &&
               q->channels[i].remote_cid==remote)result=7;
        }
        if(remote<0x40)result=6;
        if(!(psm&1) || (psm&0x100))result=2;
        if(!result) {int allow=q->authorize(q->context,rx->link,(uint16_t)psm,1);
            if(allow!=1)result=allow<0 ? 3 : 2;}
        if(!result && slot==BT_L2CAP_CHANNELS)result=4;
        unsigned local=0;
        if(!result) {
            local=0x40+slot;c=&q->channels[slot];updated=(struct bt_l2cap_channel){
                .link=rx->link,.deadline=bound,.local_cid=(uint16_t)local,.remote_cid=(uint16_t)remote,
                .psm=(uint16_t)psm,.local_mtu=q->local_mtu,.remote_mtu=672,
                .need_config=1,.state=BT_L2CAP_CONFIGURING};
        }
        put16(reply+4,local);put16(reply+6,remote);put16(reply+8,result);rn=12;
    } else if(code==4) {
        if(n<4 || (get16(p+2)&~1u))return fault(q,BT_L2CAP_PROTOCOL);
        c=channel(q,get16(p),rx->link);
        if(!c || (c->state!=BT_L2CAP_CONFIGURING && c->state!=BT_L2CAP_OPEN)) {
            rn=reject(reply,id,2,get16(p),c ? c->remote_cid : 0);c=0;
        } else {
            updated=*c;unsigned options_n=n-4,flags=get16(p+2),outn=0,result=0;
            put16(reply+4,c->remote_cid);put16(reply+6,flags);rn=10;
            if(options_n>sizeof(updated.config)-updated.config_length) {
                result=2;updated.config_length=0;
            } else {
                copy(updated.config+updated.config_length,p+4,options_n);updated.config_length+=options_n;
                /* Each fragment must contain complete options. */
                unsigned ignored=c->remote_mtu;uint8_t temporary[BT_L2CAP_SIGNAL_MTU];unsigned tempn;
                if(options(p+4,options_n,&ignored,temporary,&tempn)<0)return fault(q,BT_L2CAP_PROTOCOL);
                if(!(flags&1)) {
                    unsigned mtu=c->remote_mtu;
                    int parsed=options(updated.config,updated.config_length,&mtu,reply+10,&outn);
                    if(parsed<0)return fault(q,BT_L2CAP_PROTOCOL);result=(unsigned)parsed;
                    updated.config_length=0;
                    if(!result){updated.remote_mtu=(uint16_t)mtu;updated.output_done=0;}
                }
            }
            put16(reply+8,result);rn+=outn;ready(&updated);
        }
    } else if(code==6) {
        if(n!=4)return fault(q,BT_L2CAP_PROTOCOL);
        c=channel(q,get16(p),rx->link);
        if(!c || c->remote_cid!=get16(p+2) || c->state==BT_L2CAP_CLOSED) {
            rn=reject(reply,id,2,get16(p),get16(p+2));c=0;
        } else {copy(reply+4,p,4);rn=8;updated=*c;updated.state=BT_L2CAP_CLOSED;updated.request_id=0;}
    } else if(code==8) {unsigned echoed=n<=44 ? n : 0;copy(reply+4,p,echoed);rn=echoed+4;}
    else if(code==10) {
        if(n!=2)return fault(q,BT_L2CAP_PROTOCOL);
        unsigned type=get16(p);put16(reply+4,type);
        if(type==2){rn=12;reply[8]=0x80;}
        else if(type==3){rn=16;reply[8]=2;}
        else {rn=8;put16(reply+6,1);}
    } else rn=reject(reply,id,0,0,0);
    put16(reply+2,rn-4);
    uint64_t serial;int r=emit(q,rx->link,1,reply,rn,bound,0,&serial);
    if(r!=1)return r;
    if(c) {
        if(code==4 && !(get16(p+2)&1) && !get16(reply+8))updated.output_serial=serial;
        *c=updated;
    }
    cache->link=rx->link;cache->reply_serial=serial;cache->length=n+4;cache->reply_length=rn;
    copy(cache->request,cmd,n+4);copy(cache->reply,reply,rn);return 1;
}
static int response_code(unsigned code)
{return code==1 || code==3 || code==5 || code==7 || code==9 || code==11 ||
        code==0x13 || code==0x15 || code==0x18 || code==0x1a;}
static int signal(struct bt_l2cap *q,struct bt_l2cap_pdu *r)
{
    if(r->cursor==4) {
        /* Validate the entire bounded command list before processing its first
         * command. No partially parsed options/lengths reach channel state. */
        for(size_t at=4;at<r->length;) {
            if(r->length-at<4 || get16(r->bytes+at+2)>r->length-at-4)return fault(q,BT_L2CAP_PROTOCOL);
            at+=4+get16(r->bytes+at+2);
        }
        if(r->length-4>BT_L2CAP_SIGNAL_MTU) {
            for(size_t at=4;at<r->length;at+=4+get16(r->bytes+at+2)) {
                if(r->bytes[at+1] && !response_code(r->bytes[at])) {
                    uint8_t reply[10];unsigned n=reject(reply,r->bytes[at+1],1,0,0);
                    uint64_t bound;if(deadline(q,&bound)!=1)return -1;
                    int result=emit(q,r->link,1,reply,n,bound,0,0);if(result!=1)return result;break;
                }
            }
            r->state=BT_L2CAP_FREE;return 1;
        }
    }
    if(r->cursor==r->length){r->state=BT_L2CAP_FREE;return 1;}
    const uint8_t *cmd=r->bytes+r->cursor;unsigned n=get16(cmd+2);
    int result=1;
    if(cmd[1])result=response_code(cmd[0]) ? response(q,r,cmd,n) : peer_request(q,r,cmd,n);
    if(result==1){r->cursor+=n+4;if(r->cursor==r->length)r->state=BT_L2CAP_FREE;}
    return result;
}
static int receive_acl(struct bt_l2cap *q,const struct bt_h4_record *record)
{
    const uint8_t *p=record->storage.bytes;unsigned flags=get16(p),h=flags&0xfff,pb=(flags>>12)&3;
    size_t n=get16(p+2);unsigned li=BT_ACL_LINKS;
    if((flags&0xc000) || (pb!=1 && pb!=2) || n!=record->length-4)return fault(q,BT_L2CAP_PROTOCOL);
    for(unsigned i=0;i<BT_ACL_LINKS;++i)if(q->acl->links[i].handle==h &&
       q->acl->links[i].state==BT_ACL_CONNECTED && !q->acl->links[i].flush_seen){li=i;break;}
    if(li==BT_ACL_LINKS)return fault(q,BT_L2CAP_OWNERSHIP);
    if(!n)return bt_acl_receive_release(q->acl,record->serial);
    struct bt_l2cap_pdu *r;
    if(pb==2) {
        if(q->assembling[li]!=BT_L2CAP_PDUS)return fault(q,BT_L2CAP_PROTOCOL);
        r=free_pdu(q->rx);if(!r)return 0;
        uint64_t bound;if(deadline(q,&bound)!=1)return -1;
        if(q->next_serial==UINT64_MAX)return fault(q,BT_L2CAP_CAPACITY);
        *r=(struct bt_l2cap_pdu){.serial=q->next_serial++,.link=q->acl->links[li].generation,
            .deadline=bound,.first_record=record->serial,.state=BT_L2CAP_PARTIAL};
        q->assembling[li]=(unsigned)(r-q->rx);
    } else {
        if(q->assembling[li]==BT_L2CAP_PDUS)return fault(q,BT_L2CAP_PROTOCOL);
        r=&q->rx[q->assembling[li]];
    }
    if(n>sizeof(r->bytes)-r->length)return fault(q,BT_L2CAP_PROTOCOL);
    copy(r->bytes+r->length,p+4,n);r->length+=n;
    if(r->length>=2) {
        size_t expected=(size_t)get16(r->bytes)+4;
        if(expected>sizeof(r->bytes) || r->length>expected)return fault(q,BT_L2CAP_PROTOCOL);
        if(r->length==expected) {
            r->cid=get16(r->bytes+2);q->assembling[li]=BT_L2CAP_PDUS;
            if(r->cid==1){r->state=BT_L2CAP_SIGNAL;r->cursor=4;}
            else {
                struct bt_l2cap_channel *c=channel(q,r->cid,r->link);
                if(!c || c->state!=BT_L2CAP_OPEN)r->state=BT_L2CAP_FREE;
                else if(r->length-4>c->local_mtu)return fault(q,BT_L2CAP_PROTOCOL);
                else r->state=BT_L2CAP_RECEIVED;
            }
        }
    }
    return bt_acl_receive_release(q->acl,record->serial)==1 ? 1 : fault(q,BT_L2CAP_LOWER);
}
static int lower_results(struct bt_l2cap *q)
{
    /* Inspect submission progress without releasing the lower owner early. */
    for(unsigned i=0;i<BT_ACL_TX_SLOTS;++i)if(q->fragments[i].acl_serial) {
        struct bt_l2cap_fragment *f=&q->fragments[i];struct bt_l2cap_pdu *p=pdu(q->tx,f->pdu_serial);
        if(!p)return fault(q,BT_L2CAP_OWNERSHIP);
        for(unsigned j=0;j<BT_ACL_TX_SLOTS;++j)if(q->acl->packets[j].serial==f->acl_serial &&
            q->acl->packets[j].state!=BT_ACL_TX_FREE) {
            enum bt_acl_tx_state state=q->acl->packets[j].state;
            if(!f->accepted && (state==BT_ACL_SENT || state==BT_ACL_CREDIT_RETIRED || state==BT_ACL_FLUSHED)) {
                p->accepted+=f->length;f->accepted=1;
                if(p->accepted==p->length)for(unsigned k=0;k<BT_L2CAP_CHANNELS;++k) {
                    if(q->channels[k].request_serial==p->serial) {
                        q->channels[k].request_sent=1;
                        q->channels[k].request_after=q->acl->hci->transport.next_serial-1;
                    }
                    if(q->channels[k].output_serial==p->serial) {
                        q->channels[k].output_done=1;ready(&q->channels[k]);
                    }
                }
            }
            break;
        }
    }
    const struct bt_acl_packet *done=bt_acl_result(q->acl);
    if(done) {
        struct bt_l2cap_fragment *f=0;
        for(unsigned i=0;i<BT_ACL_TX_SLOTS;++i)if(q->fragments[i].acl_serial==done->serial){f=&q->fragments[i];break;}
        if(!f)return fault(q,BT_L2CAP_OWNERSHIP);
        struct bt_l2cap_pdu *p=pdu(q->tx,f->pdu_serial);if(!p || !p->leases)return fault(q,BT_L2CAP_OWNERSHIP);
        if(done->state!=BT_ACL_CREDIT_RETIRED || done->uncertain)p->failed=1;
        if(bt_acl_release(q->acl,done->serial)!=1)return fault(q,BT_L2CAP_LOWER);
        --p->leases;f->acl_serial=0;
    }
    for(unsigned i=0;i<BT_L2CAP_PDUS;++i) {
        struct bt_l2cap_pdu *p=&q->tx[i];if(p->state!=BT_L2CAP_TRANSMITTING)continue;
        if(!live(q,p->link)) {p->failed=1;p->cursor=p->length;}
        if(p->application)for(unsigned k=0;k<BT_L2CAP_CHANNELS;++k) {
            const struct bt_l2cap_channel *c=&q->channels[k];
            if(c->link==p->link && c->remote_cid==p->cid && c->state!=BT_L2CAP_OPEN) {
                p->failed=1;if(!p->cursor)p->cursor=p->length;
            }
        }
        if(p->cursor==p->length && !p->leases) {
            if(p->application)p->state=p->failed ? BT_L2CAP_FAILED : BT_L2CAP_COMPLETED;
            else if(p->failed)return fault(q,BT_L2CAP_LOWER);
            else p->state=BT_L2CAP_FREE;
        }
    }
    return 1;
}
static int transmit(struct bt_l2cap *q)
{
    struct bt_l2cap_pdu *p=0;
    for(unsigned i=0;i<BT_L2CAP_PDUS;++i)if(q->tx[i].state==BT_L2CAP_TRANSMITTING &&
       q->tx[i].cursor<q->tx[i].length && (!p || q->tx[i].serial<p->serial))p=&q->tx[i];
    if(!p || !live(q,p->link))return 1;
    if(p->application)for(unsigned k=0;k<BT_L2CAP_CHANNELS;++k) {
        const struct bt_l2cap_channel *c=&q->channels[k];
        if(c->link==p->link && c->remote_cid==p->cid && c->state!=BT_L2CAP_OPEN) {
            p->failed=1;
            if(!p->cursor){p->cursor=p->length;p->state=BT_L2CAP_FAILED;return 1;}
        }
    }
    struct bt_l2cap_fragment *f=0;
    for(unsigned i=0;i<BT_ACL_TX_SLOTS;++i)if(!q->fragments[i].acl_serial){f=&q->fragments[i];break;}
    if(!f)return 1;
    size_t n=p->length-p->cursor;if(n>q->acl->mtu)n=q->acl->mtu;
    uint64_t serial;int r=bt_acl_submit(q->acl,p->link,p->cursor ? 1 : 2,p->bytes+p->cursor,n,p->deadline,&serial);
    if(r<0)return fault(q,BT_L2CAP_LOWER);
    if(r==1){*f=(struct bt_l2cap_fragment){.acl_serial=serial,.pdu_serial=p->serial,.length=n};
        p->cursor+=n;++p->leases;}
    return 1;
}
int bt_l2cap_step(struct bt_l2cap *q,unsigned budget,uint64_t bound)
{
    if(!q || q->state!=BT_L2CAP_ACTIVE || !bound || !budget || budget>BT_H4_BUDGET)return -1;
    for(unsigned i=0;i<BT_L2CAP_CHANNELS;++i)if(q->channels[i].state!=BT_L2CAP_UNUSED &&
       q->channels[i].state!=BT_L2CAP_OPEN && q->channels[i].state!=BT_L2CAP_CLOSED && bound>q->channels[i].deadline)
        bound=q->channels[i].deadline;
    for(unsigned i=0;i<BT_L2CAP_PDUS;++i) {
        if(q->tx[i].state==BT_L2CAP_TRANSMITTING && bound>q->tx[i].deadline)bound=q->tx[i].deadline;
        if((q->rx[i].state==BT_L2CAP_PARTIAL || q->rx[i].state==BT_L2CAP_SIGNAL) && bound>q->rx[i].deadline)bound=q->rx[i].deadline;
    }
    if(q->acl->hci->transport.now>=bound)return fault(q,BT_L2CAP_TIMEOUT);
    if(bt_acl_step(q->acl,budget,bound)!=1)return fault(q,BT_L2CAP_LOWER);
    if(lower_results(q)!=1)return -1;
    for(unsigned i=0;i<BT_L2CAP_CHANNELS;++i)if(q->channels[i].state!=BT_L2CAP_UNUSED &&
       q->channels[i].state!=BT_L2CAP_CLOSED && !live(q,q->channels[i].link))q->channels[i].state=BT_L2CAP_CLOSED;
    const struct bt_h4_record *record=0;
    for(unsigned i=0;i<q->acl->hci->transport.count;++i) {
        const struct bt_h4_record *v=&q->acl->hci->transport.records[i];
        if(v->owner==BT_H4_OWNED && v->type==2 && v->serial<=q->acl->seen_record &&
           (!record || v->serial<record->serial))record=v;
    }
    if(record) {int result=receive_acl(q,record);if(result<0)return result;}
    struct bt_l2cap_pdu *r=0;
    for(unsigned i=0;i<BT_L2CAP_PDUS;++i)if(q->rx[i].state==BT_L2CAP_SIGNAL &&
       (!r || q->rx[i].serial<r->serial))r=&q->rx[i];
    if(r) {int result=signal(q,r);if(result<0)return result;}
    for(unsigned i=0;i<BT_L2CAP_CHANNELS;++i)if(q->channels[i].state==BT_L2CAP_CONFIGURING && q->channels[i].need_config) {
        int result=configure(q,&q->channels[i],q->channels[i].need_config==2);
        if(result<0)return fault(q,BT_L2CAP_CAPACITY);break;
    }
    return transmit(q);
}
static const struct bt_l2cap_pdu *oldest(const struct bt_l2cap_pdu *pool,unsigned receive)
{
    const struct bt_l2cap_pdu *p=0;
    for(unsigned i=0;i<BT_L2CAP_PDUS;++i)if((receive ? pool[i].state==BT_L2CAP_RECEIVED :
       pool[i].state==BT_L2CAP_COMPLETED || pool[i].state==BT_L2CAP_FAILED) && (!p || pool[i].serial<p->serial))p=&pool[i];
    return p;
}
const struct bt_l2cap_pdu *bt_l2cap_receive(const struct bt_l2cap *q)
{return q && q->state!=BT_L2CAP_OFF ? oldest(q->rx,1) : 0;}
const struct bt_l2cap_pdu *bt_l2cap_result(const struct bt_l2cap *q)
{return q && q->state!=BT_L2CAP_OFF ? oldest(q->tx,0) : 0;}
int bt_l2cap_release_receive(struct bt_l2cap *q,uint64_t serial)
{
    if(!q || q->state!=BT_L2CAP_ACTIVE)return -1;
    struct bt_l2cap_pdu *p=pdu(q->rx,serial);if(!p || p->state!=BT_L2CAP_RECEIVED)return -1;
    p->state=BT_L2CAP_FREE;return 1;
}
int bt_l2cap_release_result(struct bt_l2cap *q,uint64_t serial)
{
    if(!q || q->state!=BT_L2CAP_ACTIVE)return -1;
    struct bt_l2cap_pdu *p=pdu(q->tx,serial);if(!p || (p->state!=BT_L2CAP_COMPLETED && p->state!=BT_L2CAP_FAILED))return -1;
    p->state=BT_L2CAP_FREE;return 1;
}
const struct bt_h4_record *bt_l2cap_other(const struct bt_l2cap *q)
{
    if(!q || q->state==BT_L2CAP_OFF)return 0;const struct bt_h4_record *record=0;
    for(unsigned i=0;i<q->acl->hci->transport.count;++i) {
        const struct bt_h4_record *r=&q->acl->hci->transport.records[i];
        if(r->owner==BT_H4_OWNED && r->type!=2 && r->serial<=q->acl->seen_record &&
           (!record || r->serial<record->serial))record=r;
    }
    return record;
}
int bt_l2cap_release_other(struct bt_l2cap *q,uint64_t serial)
{
    if(!q || q->state!=BT_L2CAP_ACTIVE)return -1;
    for(unsigned i=0;i<q->acl->hci->transport.count;++i) {
        const struct bt_h4_record *r=&q->acl->hci->transport.records[i];
        if(r->serial==serial && r->type!=2 && r->owner==BT_H4_OWNED && r->serial<=q->acl->seen_record)
            return bt_acl_receive_release(q->acl,serial);
    }
    return -1;
}
