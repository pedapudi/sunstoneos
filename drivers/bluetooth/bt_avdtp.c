/* SPDX-License-Identifier: Apache-2.0 */
/* Original acceptor-only AVDTP1.3 sink for one A2DP1.3 SBC stream endpoint. */
#include "bt_avdtp.h"
#include "bt_memory.h"
enum { DISCOVER=1, GET_CAPABILITIES, SET_CONFIGURATION, GET_CONFIGURATION, RECONFIGURE, OPEN,
    START, CLOSE, SUSPEND, ABORT, SECURITY_CONTROL, GET_ALL_CAPABILITIES };
enum { GENERAL_REJECT=1, ACCEPT, REJECT };
/* AVDTP1.3 section 8.20.6 and A2DP1.3 section 5.1.3 error codes. */
enum { BAD_LENGTH=0x11, BAD_ACP_SEID=0x12, SEP_IN_USE=0x13, SEP_NOT_IN_USE=0x14,
    BAD_SERV_CATEGORY=0x17, BAD_PAYLOAD_FORMAT=0x18, INVALID_CAPABILITIES=0x1a,
    BAD_MEDIA_TRANSPORT_FORMAT=0x23, UNSUPPORTED_CONFIGURATION=0x29, BAD_STATE=0x31,
    NOT_SUPPORTED_CODEC_TYPE=0xc2, INVALID_SAMPLING_FREQUENCY=0xc3, NOT_SUPPORTED_SAMPLING_FREQUENCY=0xc4,
    INVALID_CHANNEL_MODE=0xc5, NOT_SUPPORTED_CHANNEL_MODE=0xc6, INVALID_SUBBANDS=0xc7,
    NOT_SUPPORTED_SUBBANDS=0xc8, INVALID_ALLOCATION_METHOD=0xc9, NOT_SUPPORTED_ALLOCATION_METHOD=0xca,
    INVALID_MINIMUM_BITPOOL=0xcb, NOT_SUPPORTED_MINIMUM_BITPOOL=0xcc, INVALID_MAXIMUM_BITPOOL=0xcd,
    NOT_SUPPORTED_MAXIMUM_BITPOOL=0xce, INVALID_BLOCK_LENGTH=0xdd };
static unsigned b16(const uint8_t *p){return ((unsigned)p[0]<<8)|p[1];}
static uint32_t b32(const uint8_t *p){return ((uint32_t)b16(p)<<16)|b16(p+2);}
static void cp(uint8_t *d,const uint8_t *s,size_t n){for(size_t i=0;i<n;++i)d[i]=s[i];}
static unsigned single(unsigned v){return v && !(v&(v-1));}
static uint64_t after(uint64_t now,uint64_t n){return n>UINT64_MAX-now ? UINT64_MAX : now+n;}
static int fail(struct bt_avdtp *q,enum bt_avdtp_error e){if(!q->error)q->error=e;return -1;}
/* Lower objects and receive storage are fixed once L2CAP is ACTIVE. */
static int disjoint(struct bt_avdtp *q)
{
    if(q->lower_checked)return 1;
    const struct bt_l2cap *l=q->l2cap;
    if(!bt_range_disjoint_empty(q,sizeof(*q),l,sizeof(*l)) || !bt_range_disjoint_empty(q,sizeof(*q),l->acl,sizeof(*l->acl)) ||
       !bt_range_disjoint_empty(q,sizeof(*q),l->acl->hci,sizeof(*l->acl->hci)) ||
       !bt_range_disjoint_empty(q,sizeof(*q),l->acl->hci->transport.binding.uart,sizeof(struct a733_uart)))return 0;
    for(unsigned i=0;i<l->acl->hci->transport.count;++i)
        if(!bt_range_disjoint_empty(q,sizeof(*q),l->acl->hci->transport.records[i].storage.bytes,l->acl->hci->transport.records[i].storage.capacity))return 0;
    q->lower_checked=1;return 1;
}
int bt_avdtp_init(struct bt_avdtp *q,struct bt_l2cap *l,const uint8_t *caps,uint64_t response_us,
    int (*policy)(void *,uint64_t,uint16_t,unsigned),void *context)
{
    if(!q || !l || !caps || !policy || q->initialized || l->state!=BT_L2CAP_OFF || !response_us || response_us>60000000 ||
       !bt_range_disjoint_empty(q,sizeof(*q),l,sizeof(*l)) || !bt_range_disjoint_empty(q,sizeof(*q),caps,4) ||
       (caps[0]&0x3f)!=0x3f || (caps[1]&0xff)!=0xff || caps[2]<2 || caps[3]>250 || caps[2]>caps[3])return -1;
    *q=(struct bt_avdtp){.l2cap=l,.policy=policy,.context=context,.response_us=response_us,.next_serial=1,.initialized=1};
    cp(q->capabilities,caps,4);return 1;
}
static struct bt_l2cap_channel *channel(const struct bt_avdtp *q,uint64_t identity)
{
    for(unsigned i=0;identity && i<BT_L2CAP_CHANNELS;++i)if(q->l2cap->channels[i].generation==identity &&
        q->l2cap->channels[i].state!=BT_L2CAP_UNUSED)return &q->l2cap->channels[i];
    return 0;
}
int bt_avdtp_admit(void *ctx,uint64_t link,uint16_t psm,unsigned incoming)
{
    struct bt_avdtp *q=ctx;
    if(!q || !q->initialized || q->error || q->l2cap->state!=BT_L2CAP_ACTIVE || !disjoint(q))return -1;
    int allowed=q->policy(q->context,link,psm,incoming);
    if(psm!=BT_AVDTP_PSM)return allowed;
    if(!incoming)return -1;
    if(allowed!=1)return allowed;
    struct bt_l2cap *l=q->l2cap;unsigned live=0,free=0;
    for(unsigned i=0;i<BT_ACL_LINKS;++i)if(l->acl->links[i].generation==link &&
       l->acl->links[i].state==BT_ACL_CONNECTED && !l->acl->links[i].flush_seen)live=1;
    if(!live)return -1;
    for(unsigned i=0;i<BT_L2CAP_CHANNELS;++i)if(l->channels[i].state==BT_L2CAP_UNUSED)free=1;
    if(!free || l->next_cid>0xffff || l->next_channel==UINT64_MAX || q->closing)return 0;
    if(!q->signal){q->link=link;q->signal=l->next_channel;q->stream=BT_AVDTP_IDLE;return 1;}
    const struct bt_l2cap_channel *s=channel(q,q->signal);
    /* The source opens the transport channel only after an accepted OPEN. */
    if(link==q->link && !q->media && q->stream==BT_AVDTP_OPEN && s && s->state==BT_L2CAP_OPEN){q->media=l->next_channel;return 1;}
    return 0;
}
static void reply(struct bt_avdtp *q,unsigned label,unsigned type,unsigned signal,const uint8_t *p,unsigned n)
{
    q->response[0]=(uint8_t)(label<<4|type);q->response[1]=(uint8_t)signal;cp(q->response+2,p,n);
    q->response_length=n+2;q->deadline=after(q->l2cap->acl->hci->transport.now,q->response_us);
}
static void reject(struct bt_avdtp *q,unsigned label,unsigned signal,unsigned code)
{const uint8_t p[]={(uint8_t)code};reply(q,label,REJECT,signal,p,1);}
static void reject2(struct bt_avdtp *q,unsigned label,unsigned signal,unsigned first,unsigned code)
{const uint8_t p[]={(uint8_t)first,(uint8_t)code};reply(q,label,REJECT,signal,p,2);}
/* One bit of each SBC field, contained in the capabilities, and an ordered
 * bitpool range inside the advertised range. Returns an A2DP error code. */
static unsigned sbc(const struct bt_avdtp *q,const uint8_t *c)
{
    const uint8_t *k=q->capabilities;
    if(!single(c[0]>>4))return INVALID_SAMPLING_FREQUENCY;
    if(!(c[0]&k[0]&0xf0))return NOT_SUPPORTED_SAMPLING_FREQUENCY;
    if(!single(c[0]&15))return INVALID_CHANNEL_MODE;
    if(!(c[0]&k[0]&15))return NOT_SUPPORTED_CHANNEL_MODE;
    if(!single(c[1]>>4))return INVALID_BLOCK_LENGTH;
    if(!single((c[1]>>2)&3))return INVALID_SUBBANDS;
    if(!(c[1]&k[1]&12))return NOT_SUPPORTED_SUBBANDS;
    if(!single(c[1]&3))return INVALID_ALLOCATION_METHOD;
    if(!(c[1]&k[1]&3))return NOT_SUPPORTED_ALLOCATION_METHOD;
    if(c[2]<2 || c[2]>250)return INVALID_MINIMUM_BITPOOL;
    if(c[2]<k[2])return NOT_SUPPORTED_MINIMUM_BITPOOL;
    if(c[3]>250 || c[3]<c[2])return INVALID_MAXIMUM_BITPOOL;
    if(c[3]>k[3])return NOT_SUPPORTED_MAXIMUM_BITPOOL;
    return 0;
}
/* Requested service capabilities: exactly media transport and an SBC codec.
 * On failure *category names the first offending category. */
static unsigned configuration(const struct bt_avdtp *q,const uint8_t *p,unsigned n,unsigned *category,uint8_t out[4])
{
    unsigned seen=0;*category=0;
    for(unsigned at=0;at<n;) {
        if(n-at<2 || p[at+1]>n-at-2){*category=p[at];return BAD_PAYLOAD_FORMAT;}
        unsigned kind=p[at],length=p[at+1];const uint8_t *v=p+at+2;at+=2+length;*category=kind;
        if(!kind || kind>8)return BAD_SERV_CATEGORY;
        if(seen&(1u<<kind))return BAD_PAYLOAD_FORMAT;
        seen|=1u<<kind;
        if(kind==1){if(length)return BAD_MEDIA_TRANSPORT_FORMAT;continue;}
        if(kind!=7)return UNSUPPORTED_CONFIGURATION;
        if(length<2 || v[0]!=0)return UNSUPPORTED_CONFIGURATION;
        if(v[1]!=0)return NOT_SUPPORTED_CODEC_TYPE;
        if(length!=6)return BAD_PAYLOAD_FORMAT;
        unsigned error=sbc(q,v+2);if(error)return error;
        cp(out,v+2,4);
    }
    if(!(seen&2)){*category=1;return INVALID_CAPABILITIES;}
    if(!(seen&128)){*category=7;return INVALID_CAPABILITIES;}
    return 0;
}
static void idle(struct bt_avdtp *q)
{q->stream=BT_AVDTP_IDLE;q->int_seid=0;q->sequence_valid=0;for(unsigned i=0;i<4;++i)q->configuration[i]=0;}
static void command(struct bt_avdtp *q,unsigned label,unsigned signal,const uint8_t *p,unsigned n)
{
    unsigned seid=n && (p[0]>>2)==BT_AVDTP_SEID,category=0,error;
    const struct bt_l2cap_channel *m=channel(q,q->media);
    uint8_t caps[]={1,0,7,6,0,0,0,0,0,0};
    if(signal==DISCOVER) {
        const uint8_t endpoint[]={(uint8_t)(BT_AVDTP_SEID<<2|(q->stream!=BT_AVDTP_IDLE?2:0)),8};
        if(n)reject(q,label,signal,BAD_LENGTH);else reply(q,label,ACCEPT,signal,endpoint,2);
    } else if(signal==GET_CAPABILITIES || signal==GET_ALL_CAPABILITIES) {
        if(n!=1)reject(q,label,signal,BAD_LENGTH);else if(!seid)reject(q,label,signal,BAD_ACP_SEID);
        else {cp(caps+6,q->capabilities,4);reply(q,label,ACCEPT,signal,caps,sizeof(caps));}
    } else if(signal==SET_CONFIGURATION) {
        uint8_t chosen[4];
        if(n<2)reject2(q,label,signal,0,BAD_LENGTH);else if(!seid)reject2(q,label,signal,0,BAD_ACP_SEID);
        else if(q->stream!=BT_AVDTP_IDLE)reject2(q,label,signal,0,SEP_IN_USE);
        else if(!(p[1]>>2) || (p[1]>>2)>0x3e)reject2(q,label,signal,0,BAD_PAYLOAD_FORMAT);
        else if((error=configuration(q,p+2,n-2,&category,chosen)))reject2(q,label,signal,category,error);
        else {cp(q->configuration,chosen,4);q->int_seid=p[1]>>2;q->stream=BT_AVDTP_CONFIGURED;reply(q,label,ACCEPT,signal,0,0);}
    } else if(signal==GET_CONFIGURATION) {
        if(n!=1)reject(q,label,signal,BAD_LENGTH);else if(!seid)reject(q,label,signal,BAD_ACP_SEID);
        else if(q->stream==BT_AVDTP_IDLE)reject(q,label,signal,SEP_NOT_IN_USE);
        else {cp(caps+6,q->configuration,4);reply(q,label,ACCEPT,signal,caps,sizeof(caps));}
    } else if(signal==RECONFIGURE) {
        /* Reconfiguration of an open stream is not supported. */
        if(!n)reject2(q,label,signal,0,BAD_LENGTH);else if(!seid)reject2(q,label,signal,0,BAD_ACP_SEID);
        else if(q->stream==BT_AVDTP_IDLE)reject2(q,label,signal,0,SEP_NOT_IN_USE);
        else if(q->stream!=BT_AVDTP_OPEN)reject2(q,label,signal,0,BAD_STATE);
        else reject2(q,label,signal,7,UNSUPPORTED_CONFIGURATION);
    } else if(signal==OPEN || signal==CLOSE || signal==SECURITY_CONTROL) {
        if(signal==SECURITY_CONTROL ? !n : n!=1)reject(q,label,signal,BAD_LENGTH);
        else if(!seid)reject(q,label,signal,BAD_ACP_SEID);
        else if(signal==OPEN && q->stream==BT_AVDTP_CONFIGURED){q->stream=BT_AVDTP_OPEN;reply(q,label,ACCEPT,signal,0,0);}
        else if(signal==CLOSE && (q->stream==BT_AVDTP_OPEN || q->stream==BT_AVDTP_STREAMING)) {
            if(q->media)q->stream=BT_AVDTP_CLOSING;else idle(q);reply(q,label,ACCEPT,signal,0,0);
        } else reject(q,label,signal,q->stream==BT_AVDTP_IDLE && signal==SECURITY_CONTROL?SEP_NOT_IN_USE:BAD_STATE);
    } else if(signal==START || signal==SUSPEND) {
        /* The reject names the first endpoint that failed. */
        unsigned bad=n;for(unsigned i=n;i--;)if((p[i]>>2)!=BT_AVDTP_SEID)bad=i;
        unsigned from=signal==START?BT_AVDTP_OPEN:BT_AVDTP_STREAMING;
        if(!n)reject2(q,label,signal,0,BAD_LENGTH);
        else if(bad<n)reject2(q,label,signal,p[bad],BAD_ACP_SEID);
        else if(q->stream!=from || !m || m->state!=BT_L2CAP_OPEN)reject2(q,label,signal,p[0],BAD_STATE);
        else {q->stream=signal==START?BT_AVDTP_STREAMING:BT_AVDTP_OPEN;q->sequence_valid=0;reply(q,label,ACCEPT,signal,0,0);}
    } else if(signal==ABORT) {
        /* An ABORT for another endpoint receives no response. */
        if(n==1 && seid){if(q->media)q->stream=BT_AVDTP_ABORTING;else idle(q);reply(q,label,ACCEPT,signal,0,0);}
    } else reply(q,label,GENERAL_REJECT,signal,0,0);
}
static void signaling(struct bt_avdtp *q)
{
    const struct bt_l2cap_pdu *rx=0;
    for(unsigned i=0;i<BT_L2CAP_PDUS;++i){const struct bt_l2cap_pdu *p=&q->l2cap->rx[i];
        if(p->state==BT_L2CAP_RECEIVED && p->channel==q->signal && (!rx || p->serial<rx->serial))rx=p;}
    if(!rx)return;
    const uint8_t *p=rx->bytes+4;unsigned n=(unsigned)rx->length-4;
    /* Single packets only: every supported message fits the minimum MTU.
     * Peer responses are discarded because this endpoint sends no command. */
    if(n<2 || ((p[0]>>2)&3)){fail(q,BT_AVDTP_PROTOCOL);return;}
    if(!(p[0]&3) && !q->closing)command(q,p[0]>>4,p[1]&0x3f,p+2,n-2);
    if(bt_l2cap_release_receive(q->l2cap,rx->serial)!=1)fail(q,BT_AVDTP_LOWER);
}
/* SBC frame length from A2DP1.3 section 12.9 after matching every header
 * field to the configuration; zero for a mismatch. */
static unsigned frame(const struct bt_avdtp *q,const uint8_t *h,unsigned n)
{
    const uint8_t *c=q->configuration;
    if(n<4 || h[0]!=0x9c)return 0;
    unsigned f=h[1]>>6,b=(h[1]>>4)&3,mode=(h[1]>>2)&3,snr=(h[1]>>1)&1,s=h[1]&1,pool=h[2];
    if(!(c[0]&(0x80u>>f)) || !(c[1]&(0x80u>>b)) || !(c[0]&(8u>>mode)) || !(c[1]&(snr?2u:1u)) ||
       !(c[1]&(s?4u:8u)) || pool<c[2] || pool>c[3])return 0;
    unsigned blocks=4*(b+1),subbands=s?8:4,channels=mode?2:1;
    unsigned length=4+(4*subbands*channels)/8;
    if(mode<2)length+=(blocks*channels*pool+7)/8;
    else length+=((mode==3?subbands:0)+blocks*pool+7)/8;
    return length<=n?length:0;
}
static void media(struct bt_avdtp *q)
{
    const struct bt_l2cap_pdu *rx=0;
    for(unsigned i=0;q->media && i<BT_L2CAP_PDUS;++i){const struct bt_l2cap_pdu *p=&q->l2cap->rx[i];
        if(p->state==BT_L2CAP_RECEIVED && p->channel==q->media && (!rx || p->serial<rx->serial))rx=p;}
    if(!rx)return;
    const uint8_t *p=rx->bytes+4;unsigned n=(unsigned)rx->length-4,at=12,end=n,valid=0,frames=0;
    struct bt_avdtp_media *slot=0;uint16_t sequence=0;unsigned gap=0;
    for(unsigned i=0;i<BT_AVDTP_SLOTS;++i)if(!q->slots[i].published){slot=&q->slots[i];break;}
    if(q->stream==BT_AVDTP_STREAMING && !q->closing && n>=13 && (p[0]>>6)==2) {
        /* RTP: skip CSRCs and an extension, remove padding, then the media
         * payload header; fragmented SBC frames are not supported. */
        at+=4*(p[0]&15);sequence=(uint16_t)b16(p+2);
        if((p[0]&16) && at+4<=n)at+=4+4*b16(p+at+2);else if(p[0]&16)at=n;
        if((p[0]&32) && n && p[n-1] && p[n-1]<=n)end=n-p[n-1];else if(p[0]&32)end=0;
        gap=q->sequence_valid?(uint16_t)(sequence-q->sequence):0;
        if(at<end && !(p[at]&0x80) && (p[at]&15) && gap<0x8000) {
            frames=p[at++]&15;unsigned cursor=at;valid=1;
            for(unsigned i=0;i<frames && valid;++i){unsigned length=frame(q,p+cursor,end-cursor);
                if(!length)valid=0;cursor+=length;}
            if(cursor!=end || end-at>BT_AVDTP_PAYLOAD)valid=0;
        }
        if(!valid)++q->invalid;else if(!slot){++q->dropped;valid=0;}
    }
    if(valid) {
        *slot=(struct bt_avdtp_media){.serial=q->next_serial++,.pdu=rx->serial,.timestamp=b32(p+4),.sequence=sequence,
            .frames=frames,.length=end-at,.gap=gap,.published=1};
        cp(slot->bytes,p+at,end-at);q->gaps+=gap;q->sequence=(uint16_t)(sequence+1);q->sequence_valid=1;
    }
    if(bt_l2cap_release_receive(q->l2cap,rx->serial)!=1)fail(q,BT_AVDTP_LOWER);
}
/* Confirmed closure releases a channel after its received PDUs. */
static int release(struct bt_avdtp *q,uint64_t identity)
{
    for(unsigned i=0;i<BT_L2CAP_PDUS;++i)if(q->l2cap->rx[i].state==BT_L2CAP_RECEIVED && q->l2cap->rx[i].channel==identity &&
        bt_l2cap_release_receive(q->l2cap,q->l2cap->rx[i].serial)!=1)return fail(q,BT_AVDTP_LOWER);
    int r=bt_l2cap_release_channel(q->l2cap,identity);return r<0?fail(q,BT_AVDTP_LOWER):r;
}
static int maintain(struct bt_avdtp *q)
{
    uint64_t now=q->l2cap->acl->hci->transport.now;
    struct bt_l2cap_channel *s=channel(q,q->signal),*m=channel(q,q->media);
    if((q->signal && (!s || s->link!=q->link || s->psm!=BT_AVDTP_PSM)) ||
       (q->media && (!m || m->link!=q->link || m->psm!=BT_AVDTP_PSM)))return fail(q,BT_AVDTP_IDENTITY);
    if((s && s->state==BT_L2CAP_QUARANTINED) || (m && m->state==BT_L2CAP_QUARANTINED))return fail(q,BT_AVDTP_LOWER);
    if(q->tx_serial) {
        const struct bt_l2cap_pdu *p=0;
        for(unsigned i=0;i<BT_L2CAP_PDUS;++i)if(q->l2cap->tx[i].state!=BT_L2CAP_FREE && q->l2cap->tx[i].serial==q->tx_serial)p=&q->l2cap->tx[i];
        if(!p || p->channel!=q->signal)return fail(q,BT_AVDTP_IDENTITY);
        /* A response on a confirmed closed channel ends without delivery. */
        if(p->state==BT_L2CAP_FAILED && !(s->state==BT_L2CAP_CLOSED && !p->leases))return fail(q,BT_AVDTP_LOWER);
        if(p->state==BT_L2CAP_COMPLETED || p->state==BT_L2CAP_FAILED) {
            if(bt_l2cap_release_result(q->l2cap,p->serial)!=1)return fail(q,BT_AVDTP_LOWER);q->tx_serial=0;}
    }
    if(s && s->state==BT_L2CAP_CLOSED)q->response_length=0;
    if((q->tx_serial || q->response_length) && now>=q->deadline)return fail(q,BT_AVDTP_EXPIRED);
    if(m && m->state==BT_L2CAP_CLOSED) {
        int r=release(q,q->media);if(r<0)return -1;
        if(r==1){q->media=0;m=0;idle(q);}
    }
    /* Signaling closure by the peer, or local close, ends the stream. */
    unsigned ending=q->closing || (s && s->state==BT_L2CAP_CLOSED);
    if(ending && !q->close_deadline)q->close_deadline=after(now,q->response_us);
    if(ending && now>=q->close_deadline && (q->signal || q->media))return fail(q,BT_AVDTP_EXPIRED);
    if(ending && m && m->state==BT_L2CAP_OPEN && bt_l2cap_close(q->l2cap,q->media,q->close_deadline)<0)return fail(q,BT_AVDTP_LOWER);
    if(ending && !m && s && s->state==BT_L2CAP_OPEN && !q->tx_serial && bt_l2cap_close(q->l2cap,q->signal,q->close_deadline)<0)return fail(q,BT_AVDTP_LOWER);
    if(!m && s && s->state==BT_L2CAP_CLOSED && !q->tx_serial) {
        int r=release(q,q->signal);if(r<0)return -1;
        if(r==1){q->signal=0;q->link=0;idle(q);if(!q->closing)q->close_deadline=0;}
    }
    return 1;
}
int bt_avdtp_step(struct bt_avdtp *q,unsigned budget,uint64_t bound)
{
    if(!q || !q->initialized || q->error)return -1;
    if(q->l2cap->state!=BT_L2CAP_ACTIVE || !disjoint(q) || bt_l2cap_step(q->l2cap,budget,bound)!=1)return fail(q,BT_AVDTP_LOWER);
    if(maintain(q)!=1)return -1;
    const struct bt_l2cap_channel *s=channel(q,q->signal);
    if(s && s->state==BT_L2CAP_OPEN && !q->tx_serial && !q->response_length)signaling(q);
    if(!q->error && q->response_length && !q->tx_serial && s && s->state==BT_L2CAP_OPEN) {
        /* Accepted lower bytes keep the independent ERTX transport bound,
         * so the response deadline expires here before lower ownership. */
        uint64_t serial=0,transport=after(q->l2cap->acl->hci->transport.now,q->l2cap->ertx_us);
        int r=bt_l2cap_send(q->l2cap,q->signal,q->response,q->response_length,transport,&serial);
        if(r<0)return fail(q,BT_AVDTP_LOWER);
        if(r==1){q->tx_serial=serial;q->response_length=0;}
    }
    if(!q->error)media(q);
    if(q->error)return -1;
    return q->closing && !q->signal && !q->media ? 0 : 1;
}
const struct bt_avdtp_media *bt_avdtp_media(const struct bt_avdtp *q)
{
    const struct bt_avdtp_media *m=0;
    for(unsigned i=0;q && q->initialized && i<BT_AVDTP_SLOTS;++i)if(q->slots[i].published && (!m || q->slots[i].serial<m->serial))m=&q->slots[i];
    return m;
}
int bt_avdtp_release_media(struct bt_avdtp *q,uint64_t serial)
{
    for(unsigned i=0;q && q->initialized && serial && i<BT_AVDTP_SLOTS;++i)
        if(q->slots[i].published && q->slots[i].serial==serial){q->slots[i].published=0;return 1;}
    return -1;
}
int bt_avdtp_close(struct bt_avdtp *q,uint64_t bound)
{
    if(!q || !q->initialized || q->error || q->closing || bound<=q->l2cap->acl->hci->transport.now)return -1;
    q->closing=1;q->close_deadline=bound;q->response_length=0;return 1;
}
