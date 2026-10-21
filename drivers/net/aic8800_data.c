/* Native packet worker; source ABI and supported receive profile are documented
 * in docs/radio-data.md. No allocation, Linux code, retry loop or key handling. */
#include "aic8800_data.h"
#include "aic8800_security.h"
#include <string.h>
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0]|(uint16_t)p[1]<<8); }
static uint16_t be16(const uint8_t *p) { return (uint16_t)((uint16_t)p[0]<<8|p[1]); }
static uint32_t le32(const uint8_t *p) { return le16(p)|(uint32_t)le16(p+2)<<16; }
static void put16(uint8_t *p,uint16_t n) { p[0]=(uint8_t)n;p[1]=(uint8_t)(n>>8); }
static void put32(uint8_t *p,uint32_t n) { put16(p,(uint16_t)n);put16(p+2,(uint16_t)(n>>16)); }
static int equal(const uint8_t *a,const uint8_t *b,unsigned n)
{ for(unsigned i=0;i<n;++i)if(a[i]!=b[i])return 0;return 1; }
static int linked(const struct aic8800_data *d)
{
    const struct aic8800_wifi *w=d->wifi;
    return w&&w->state==AIC8800_WIFI_READY&&w->association_generation&&
        (w->link==AIC8800_WIFI_ASSOCIATED_OPEN||w->link==AIC8800_WIFI_SECURITY_PENDING||w->link==AIC8800_WIFI_SECURED);
}
static int active(const struct aic8800_data *d)
{ return d->state==AIC8800_DATA_REQUEST||d->state==AIC8800_DATA_WAITING; }
static int port_open(const struct aic8800_data *d)
{ return linked(d)&&((d->wifi->link==AIC8800_WIFI_ASSOCIATED_OPEN&&d->open_authority&&
    d->open_generation==d->wifi->association_generation)||aic8800_security_open(d->security)); }
static int eapol(const uint8_t *p,size_t n)
{ return n>=4&&be16(p+2)<=n-4; }
int aic8800_data_init(struct aic8800_data *d)
{
    if(!d||d->initialized)return 0;
    struct aic8800_data fresh={0};fresh.initialized=1;*d=fresh;return 1;
}
int aic8800_data_claim(struct aic8800_data *d,const void *owner,const struct aic8800_wifi *w,uint64_t now)
{
    if(!d||!d->initialized||d->owner||!owner||!w||w->owner!=owner||d->state!=AIC8800_DATA_IDLE)return 0;
    d->owner=owner;d->wifi=w;d->last_now=now;return 1;
}
void aic8800_data_stop(struct aic8800_data *d,const void *owner,enum aic8800_data_error e)
{ if(d&&d->initialized&&d->owner==owner&&d->state!=AIC8800_DATA_FAULT){d->error=e;d->state=AIC8800_DATA_FAULT;} }
void aic8800_data_observe(struct aic8800_data *d,const void *owner,uint64_t now)
{
    if(!d||!d->initialized||d->owner!=owner||d->state==AIC8800_DATA_FAULT)return;
    if(now<d->last_now||(active(d)&&now>=d->deadline)||
        (d->completion_deadline&&now>=d->completion_deadline)) {
        aic8800_data_stop(d,owner,AIC8800_DATA_TIME_ERROR);return;
    }
    d->last_now=now;d->completion_deadline=0;
    if(!linked(d)||d->open_generation!=d->wifi->association_generation)d->open_authority=0;
    if(active(d)&&(!linked(d)||d->result.generation!=d->wifi->association_generation))
        aic8800_data_stop(d,owner,AIC8800_DATA_LINK_ERROR);
}
int aic8800_data_authorize_open(struct aic8800_data *d,const void *owner,uint64_t record)
{
    if(!d||!d->initialized||d->owner!=owner||d->state!=AIC8800_DATA_IDLE||!record||
        !linked(d)||d->wifi->link!=AIC8800_WIFI_ASSOCIATED_OPEN)return 0;
    d->open_authority=record;d->open_generation=d->wifi->association_generation;return 1;
}
int aic8800_data_send(struct aic8800_data *d,const void *owner,const uint8_t *p,size_t n,uint64_t now,uint64_t deadline)
{
    if(!d||!d->initialized||d->owner!=owner||d->state!=AIC8800_DATA_IDLE||!p||n<14||
        n>AIC8800_DATA_ETHERNET_BYTES||now<d->last_now||deadline<=now||!linked(d)||!equal(p+6,d->wifi->mac,6))return 0;
    unsigned control=be16(p+12)==0x888e;
    const uint8_t pae[]={1,0x80,0xc2,0,0,3};
    if(control) {
        if(!eapol(p+14,n-14)||(!equal(p,d->wifi->target.bssid,6)&&!equal(p,pae,6))||
            d->next_sequence>=0x80000000u)return 0;
    }else if(!port_open(d)||be16(p+12)<1536)return 0;
    d->result=(struct aic8800_data_result){.generation=d->wifi->association_generation,.eapol=(uint8_t)control};
    if(control)d->result.sequence=d->next_sequence++;
    memset(d->request,0,sizeof(d->request));
    uint8_t *q=d->request;size_t payload=n-14,length=28+payload;
    put16(q,(uint16_t)length);q[2]=1;q[3]=aic8800_sdio_header_crc(q);
    put16(q+4,(uint16_t)payload);if(control)put32(q+8,0x80000000u|d->result.sequence);
    memcpy(q+12,p,14);
    q[26]=1; /* AC_BE */ q[27]=d->wifi->peer_qos?0:0xff;
    q[28]=d->wifi->vif;q[29]=d->wifi->ap_index;
    memcpy(q+32,p+14,payload);
    size_t bytes=(4+length+3)&~(size_t)3;
    if(bytes%512)bytes+=4;
    d->request_bytes=(bytes+511)&~(size_t)511;
    d->last_now=now;d->deadline=deadline;d->completion_deadline=0;d->state=AIC8800_DATA_REQUEST;return 1;
}
int aic8800_data_sent(struct aic8800_data *d,const void *owner)
{
    if(!d||d->owner!=owner||d->state!=AIC8800_DATA_REQUEST)return 0;
    d->result.bus_transferred=1;
    if(d->result.eapol)d->state=AIC8800_DATA_WAITING;
    else {d->state=AIC8800_DATA_DONE;d->completion_deadline=d->deadline;}
    return 1;
}
/* One A-MSDU subframe at MPDU offset at: DA, SA, big-endian MSDU length, then
 * an RFC 1042 SNAP MSDU with an ordinary EtherType. Every subframe except the
 * last is padded to four bytes, as in the reference receive path. Returns the
 * next subframe offset (end after the last), or 0 for a malformed subframe.
 * The DA must be local or a group address. This also rejects the forged
 * A-MSDU of CVE-2020-24588: when a plain MSDU's A-MSDU bit is set, its SNAP
 * header aa:aa:03:00:00:00 reads as a foreign unicast DA. Relaxing the DA
 * check would reopen that attack. */
static size_t subframe(const struct aic8800_data *d,const uint8_t *m,size_t at,size_t end)
{
    const uint8_t snap[]={0xaa,0xaa,3,0,0,0};
    if(end-at<22)return 0;
    size_t length=be16(m+at+12),used=14+length;
    if(length<8||length>end-at-14||length+6>AIC8800_DATA_ETHERNET_BYTES||!equal(m+at+14,snap,6)||
        be16(m+at+20)<1536||be16(m+at+20)==0x888e||(!(m[at]&1)&&!equal(m+at,d->wifi->mac,6)))return 0;
    if(used==end-at)return end;
    used=(used+3)&~(size_t)3;
    return used<=end-at?at+used:0;
}
static void deliver_subframe(struct aic8800_data *d)
{
    const uint8_t *m=d->amsdu;size_t at=d->amsdu_next,length=be16(m+at+12);
    memcpy(d->ethernet,m+at,12);memcpy(d->ethernet+12,m+at+20,length-6);
    d->frame.bytes=(uint16_t)(length+6);d->amsdu_next=subframe(d,m,at,d->amsdu_end);
}
enum aic8800_codec_result aic8800_data_response(struct aic8800_data *d,const void *owner,
    const struct aic8800_sdio_response *r,uint64_t now)
{
    if(!d||d->owner!=owner||!r)return AIC8800_CODEC_UNRELATED;
    aic8800_data_observe(d,owner,now);
    if(d->state==AIC8800_DATA_FAULT)return AIC8800_CODEC_MALFORMED;
    const uint8_t *p=r->record;
    if((r->transport_type&0x7f)==0x12) {
        if(!p||r->record_bytes!=12||d->state!=AIC8800_DATA_WAITING)return AIC8800_CODEC_UNRELATED;
        uint32_t status=le32(p+4),index=le32(p+8);
        /* Reference indexes modulo 256, which discards the hostid flag. Keep
         * all 31 sequence bits; bit31 may be retained or stripped by firmware. */
        if((index&0x7fffffffu)!=d->result.sequence||!status)return AIC8800_CODEC_UNRELATED;
        d->result.raw_status=status;d->result.raw_index=index;d->result.firmware_status_valid=1;
        d->result.tx_done=(uint8_t)(status&1);d->result.retry_required=(uint8_t)((status>>1)&1);
        d->result.software_retry_required=(uint8_t)((status>>2)&1);d->result.acknowledged=(uint8_t)((status>>3)&1);
        d->state=AIC8800_DATA_DONE;d->completion_deadline=d->deadline;return AIC8800_CODEC_OK;
    }
    if((r->transport_type&0x10)||!p||r->record_bytes<60||d->held||!linked(d))return AIC8800_CODEC_UNRELATED;
    size_t n=le16(p);if(n!=r->record_bytes-60||n<24)return AIC8800_CODEC_UNRELATED;
    uint32_t flags=le32(p+48),status=le32(p+36);
    unsigned encrypted=((status>>2)&7)==3;
    /* upload required; management, four-address and monitor records stay
     * opaque. Error/decryption bits are not fabricated as success. Records that
     * need reordering are delivered in arrival order; the CCMP replay check then
     * drops any older packet number. As in the reference SDIO receive path,
     * A-MSDU follows the QoS control bit; a set hardware bit without it is refused. */
    if(!(flags&0x40)||(flags&0x86)||p[49]!=d->wifi->vif||
        (p[50]!=d->wifi->ap_index&&p[50]!=0xff)||(status&0x3e0)||
        ((status&0x1c)&&!encrypted))return AIC8800_CODEC_UNRELATED;
    const uint8_t *m=p+60;uint16_t fc=le16(m);
    if((fc&0x000f)!=8||(fc&0x0300)!=0x0200||(fc&0x8400)||
        (!!(fc&0x4000)!=encrypted)||
        (le16(m+22)&15)||!equal(m+10,d->wifi->target.bssid,6)||
        (!(m[4]&1)&&!equal(m+4,d->wifi->mac,6)))return AIC8800_CODEC_UNRELATED;
    /* Only data or QoS data; null/CF variants are not Ethernet MSDUs. */
    if((fc&0x00f0)!=0&&(fc&0x00f0)!=0x0080)return AIC8800_CODEC_UNRELATED;
    size_t header=(fc&0x0080)?26:24;
    unsigned amsdu=(fc&0x0080)&&(m[24]&0x80);
    if(n<header+8||((flags&1)&&!amsdu))return AIC8800_CODEC_UNRELATED;
    unsigned key_slot=0,tid=(fc&0x80)?m[24]&15:16;uint64_t pn=0;
    if(encrypted) {
        if(n<header+16)return AIC8800_CODEC_UNRELATED;
        const uint8_t *ccmp=m+header;
        if(ccmp[2]||(ccmp[3]&0x3f)!=0x20)return AIC8800_CODEC_UNRELATED;
        pn=(uint64_t)ccmp[0]|(uint64_t)ccmp[1]<<8;
        for(unsigned i=0;i<4;++i)pn|=(uint64_t)ccmp[4+i]<<(16+8*i);
        if(!aic8800_security_rx_key(d->security,!!(m[4]&1),ccmp[3]>>6,tid,pn,&key_slot))return AIC8800_CODEC_UNRELATED;
        header+=8;
    }
    if(amsdu) {
        /* Validate every subframe before committing the MPDU's packet number.
         * EAPOL is never accepted inside an aggregate. */
        if(!port_open(d)||(d->wifi->target.security==AIC8800_WIFI_WPA2_PSK_CCMP&&!encrypted))return AIC8800_CODEC_UNRELATED;
        for(size_t at=header;at<n;)if(!(at=subframe(d,m,at,n)))return AIC8800_CODEC_UNRELATED;
        d->frame=(struct aic8800_data_frame){.generation=d->wifi->association_generation,.ethernet=d->ethernet,
            .encrypted=(uint8_t)encrypted,.vif=d->wifi->vif,.ap_index=d->wifi->ap_index};
        d->amsdu=m;d->amsdu_next=header;d->amsdu_end=n;deliver_subframe(d);
        if(encrypted)aic8800_security_rx_commit(d->security,key_slot,tid,pn);
        d->held_record=r->record;d->held=1;return AIC8800_CODEC_UNRELATED;
    }
    const uint8_t snap[]={0xaa,0xaa,3,0,0,0};
    if(!equal(m+header,snap,6))return AIC8800_CODEC_UNRELATED;
    size_t bytes=n-header-8+14;if(bytes>AIC8800_DATA_ETHERNET_BYTES)return AIC8800_CODEC_UNRELATED;
    unsigned control=be16(m+header+6)==0x888e;
    if(control) {if(!eapol(m+header+8,n-header-8))return AIC8800_CODEC_UNRELATED;}
    else if(!port_open(d)||be16(m+header+6)<1536||
        (d->wifi->target.security==AIC8800_WIFI_WPA2_PSK_CCMP&&!encrypted))return AIC8800_CODEC_UNRELATED;
    for(unsigned i=0;i<6;++i){d->ethernet[i]=m[4+i];d->ethernet[6+i]=m[16+i];}
    memcpy(d->ethernet+12,m+header+6,bytes-12);
    d->frame=(struct aic8800_data_frame){.generation=d->wifi->association_generation,.ethernet=d->ethernet,
        .bytes=(uint16_t)bytes,.eapol=(uint8_t)control,.encrypted=(uint8_t)encrypted,.vif=d->wifi->vif,.ap_index=d->wifi->ap_index};
    if(encrypted)aic8800_security_rx_commit(d->security,key_slot,tid,pn);
    d->held_record=r->record;d->held=1;return AIC8800_CODEC_UNRELATED;
}
void aic8800_data_consumed(struct aic8800_data *d,const void *owner,const uint8_t *record)
{
    if(!d||d->owner!=owner||d->state==AIC8800_DATA_FAULT||d->held_record!=record)return;
    /* The next A-MSDU subframe replaces the consumed view; the record stays held. */
    if(d->amsdu&&d->amsdu_next<d->amsdu_end){deliver_subframe(d);return;}
    d->held=0;d->held_record=0;d->amsdu=0;
}
