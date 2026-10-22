/* Protocol routing follows the pinned SDIO receive and outstanding-command
 * dispatch described in docs/radio-dispatch.md. No reference code is copied. */
#include "aic8800_dispatch.h"
#include "aic8800_runtime.h"
#include "aic8800_security.h"
#include "aic8800_supplicant.h"
#include "aic8800_firmware.h"
#include "a733_range.h"
#include <string.h>
static uint16_t le16(const uint8_t *p){return (uint16_t)((unsigned)p[0]|(unsigned)p[1]<<8);}
static uint32_t le32(const uint8_t *p){return (uint32_t)le16(p)|(uint32_t)le16(p+2)<<16;}
static void increment(uint64_t *v){if(*v!=UINT64_MAX)++*v;}
static int independent(const struct aic8800_dispatch *d,const void *p,size_t n)
{
    const struct aic8800_sdio_transport *t=d->transport;
    if(!a733_ranges_disjoint_optional_second(p,n,d,sizeof(*d))||!a733_ranges_disjoint_optional_second(p,n,t,sizeof(*t))||
        !a733_ranges_disjoint_optional_second(p,n,t->card,sizeof(*t->card))||
        !a733_ranges_disjoint_optional_second(p,n,t->card->host,sizeof(*t->card->host))||
        !a733_ranges_disjoint_optional_second(p,n,t->runtime,sizeof(struct aic8800_runtime))||
        !a733_ranges_disjoint_optional_second(p,n,t->wifi,sizeof(struct aic8800_wifi))||
        !a733_ranges_disjoint_optional_second(p,n,t->data,sizeof(struct aic8800_data))||
        !a733_ranges_disjoint_optional_second(p,n,t->security,sizeof(struct aic8800_security))||
        !a733_ranges_disjoint_optional_second(p,n,t->supplicant,sizeof(struct aic8800_supplicant))||
        !a733_ranges_disjoint_optional_second(p,n,t->firmware,sizeof(struct aic8800_firmware)))return 0;
    if(t->firmware)for(unsigned i=0;i<AIC8800_FIRMWARE_BLOBS;++i)
        if(!a733_ranges_disjoint_optional_second(p,n,t->firmware->config.blobs[i].data,t->firmware->config.blobs[i].bytes))return 0;
    return 1;
}
static void fault(struct aic8800_dispatch *d)
{d->faulted=1;aic8800_transport_fault_receive(d->transport,d);}
static uint64_t generation(const struct aic8800_dispatch *d)
{return d->transport->wifi?d->transport->wifi->association_generation:0;}
static int sequence(struct aic8800_dispatch *d,uint64_t *out)
{if(d->next_sequence==UINT64_MAX){fault(d);return 0;}*out=++d->next_sequence;return 1;}
int aic8800_dispatch_init(struct aic8800_dispatch *d,struct aic8800_sdio_transport *t)
{
    if(!d||!t||!t->initialized||d->initialized||t->receive_owner)return 0;
    /* The transport validates the complete owner span before any object write. */
    if(!aic8800_transport_claim_receive(t,d,sizeof(*d)))return 0;
    memset(d,0,sizeof(*d));d->initialized=1;d->transport=t;return 1;
}
static int claimed_message(const struct aic8800_dispatch *d,uint16_t id,const uint8_t *p,size_t bytes)
{
    const struct aic8800_sdio_transport *t=d->transport;
    if(t->runtime&&((id==0x0417&&!t->runtime->binding_seen)||
        (id==0x0416&&t->runtime->state==AIC8800_RUNTIME_WAITING)))return 1;
    if(t->security&&t->security->state==AIC8800_SECURITY_WAITING&&id==t->security->expected_cfm)return 1;
    const struct aic8800_wifi *w=t->wifi;if(!w)return 0;
    if(id==0x1802){
        if(bytes!=852)return 1;
        if(p[9]!=w->vif)return 0;
        if(p[8])return 1; /* Unsupported roaming remains unresolved. */
        return w->state==AIC8800_WIFI_WAITING&&w->expected_cfm==0x1801&&!w->station_indicated;
    }
    if(id==0x1805){
        if(bytes!=6)return 1;
        return p[2]==w->vif&&w->link!=AIC8800_WIFI_LINK_DOWN&&
            !(w->link==AIC8800_WIFI_CONNECTING&&w->state!=AIC8800_WIFI_WAITING);
    }
    if(w->state!=AIC8800_WIFI_WAITING)return 0;
    if(w->expected_cfm==0x1009&&id==0x1001)
        return bytes!=3||(p[0]==w->vif&&!w->scan_done);
    if(w->expected_cfm==0x1009&&id==0x1004)
        return bytes==SIZE_MAX||bytes<12||p[8]==w->vif||p[8]==255;
    if(id!=w->expected_cfm)return 0;
    if(id==0x1009&&w->summary.accepted)return 0;
    if((id==0x1801||id==0x1804)&&w->station_accepted)return 0;
    return 1;
}
static int diagnostic(struct aic8800_dispatch *d,const struct aic8800_sdio_response *r,
    enum aic8800_dispatch_record_kind kind)
{
    if(r->record_bytes>AIC8800_TRANSPORT_RX_BYTES){fault(d);return 0;}
    if(d->record_count==AIC8800_DISPATCH_RECORDS){increment(&d->discarded_diagnostics);return 1;}
    struct aic8800_dispatch_record *out=&d->records[(d->record_head+d->record_count)%AIC8800_DISPATCH_RECORDS];
    uint64_t serial;if(!sequence(d,&serial))return 0;
    memset(out,0,sizeof(*out));out->sequence=serial;out->generation=generation(d);out->kind=kind;
    size_t bytes=r->record_bytes>sizeof(out->payload)?sizeof(out->payload):r->record_bytes;
    out->bytes=(uint16_t)bytes;out->truncated=bytes<r->record_bytes;out->message_id=r->message_id;out->has_message=r->has_message;
    out->transport_type=r->transport_type;memcpy(out->payload,r->record,bytes);++d->record_count;return 1;
}
/* Return 1 when this step retired a provably unclaimed complete command record.
 * Partial/ambiguous headers remain with the original parser and terminal fault. */
static int before_parse(struct aic8800_dispatch *d)
{
    const uint8_t *p;size_t available;struct aic8800_sdio_transport *t=d->transport;
    if(!aic8800_transport_receive_view(t,d,&p,&available)||available<4)return 0;
    size_t length=le16(p),consumed=4+((length+3)&~(size_t)3);
    if(!length||(p[2]&0x7f)!=0x11||length<12||consumed>available)return 0;
    uint16_t id=le16(p+4),parameters=le16(p+10);
    /* Invalid declared parameters must never supply unchecked VIF offsets. */
    int valid=parameters<=AIC8800_SDIO_MAX_PARAMETERS&&parameters<=length-12;
    if(claimed_message(d,id,valid?p+16:NULL,valid?parameters:SIZE_MAX))return 0;
    struct aic8800_sdio_response r={.record=p,.record_bytes=length+4,.consumed=consumed,
        .transport_type=p[2],.has_message=1,.message_id=id};
    if(!diagnostic(d,&r,valid?AIC8800_DISPATCH_UNCLAIMED:AIC8800_DISPATCH_MALFORMED_UNCLAIMED))return 1;
    if(!aic8800_transport_retire_record(t,d,p,consumed))fault(d);else increment(&d->retired_records);
    return 1;
}
static int event(struct aic8800_dispatch *d,const struct aic8800_sdio_response *r)
{
    struct aic8800_sdio_transport *t=d->transport;enum aic8800_dispatch_event_kind kind;
    if(t->wifi&&t->wifi->station_held&&t->wifi->station_record==r->record)kind=AIC8800_DISPATCH_STATION;
    else if(t->wifi&&t->wifi->held&&t->wifi->held_record==r->record)kind=AIC8800_DISPATCH_SCAN;
    else if(t->data&&t->data->held&&t->data->held_record==r->record&&t->data->frame.eapol)kind=AIC8800_DISPATCH_EAPOL;
    else return 0;
    if(d->terminal_event_record==r->record)return 1;
    if(d->event_count==AIC8800_DISPATCH_EVENTS)return 1;
    struct aic8800_dispatch_event *out=&d->events[(d->event_head+d->event_count)%AIC8800_DISPATCH_EVENTS];
    uint64_t serial;if(!sequence(d,&serial))return 1;
    memset(out,0,sizeof(*out));out->sequence=serial;out->generation=generation(d);out->kind=kind;
    if(kind==AIC8800_DISPATCH_STATION){out->metadata.station=t->wifi->station_event;out->metadata.station.payload=NULL;
        out->bytes=t->wifi->station_event.bytes;memcpy(out->payload,t->wifi->station_event.payload,out->bytes);}
    else if(kind==AIC8800_DISPATCH_SCAN){out->metadata.scan=t->wifi->result;out->metadata.scan.frame=NULL;
        out->bytes=t->wifi->result.frame_bytes;memcpy(out->payload,t->wifi->result.frame,out->bytes);}
    else{out->bytes=t->data->frame.bytes;memcpy(out->payload,t->data->frame.ethernet,out->bytes);}
    ++d->event_count;
    if(t->state==AIC8800_TRANSPORT_FAULT)d->terminal_event_record=r->record;
    else if(!aic8800_transport_consume_receive(t,d)){d->terminal_event_record=r->record;fault(d);}
    return 1;
}
void aic8800_dispatch_poll(struct aic8800_dispatch *d)
{
    if(!d||!d->initialized||d->entered||d->faulted)return;
    struct aic8800_sdio_transport *t=d->transport;d->entered=1;
    if(t->receive_owner!=d){fault(d);goto out;}
    if(before_parse(d))goto out;
    aic8800_transport_poll_receive(t,d);
    if(t->state==AIC8800_TRANSPORT_FAULT){
        d->faulted=1;struct aic8800_sdio_response terminal;
        if(aic8800_transport_peek(t,&terminal))(void)event(d,&terminal);
        goto out;
    }
    struct aic8800_sdio_response r;
    if(!aic8800_transport_peek(t,&r))goto out;
    if(event(d,&r))goto out;
    struct aic8800_data_frame frame;
    if(aic8800_transport_peek_data(t,&frame))goto out; /* Actual Ethernet consumer owns the record. */
    if(r.has_message&&claimed_message(d,r.message_id,r.parameters,r.parameter_bytes)){
        /* An owned response which did not complete or publish an event cannot
         * be silently retired as an unrelated record. */
        fault(d);goto out;
    }
    if((r.transport_type&0x7f)==0x12&&t->data&&t->data->state==AIC8800_DATA_WAITING&&
        (r.record_bytes<12||(le32(r.record+8)&0x7fffffff)==t->data->result.sequence)){
        fault(d);goto out;
    }
    enum aic8800_dispatch_record_kind kind=!(r.transport_type&0x10)?AIC8800_DISPATCH_UNSUPPORTED_DATA:
        (r.transport_type&0x7f)==0x13?AIC8800_DISPATCH_PRINT:AIC8800_DISPATCH_UNCLAIMED;
    if(diagnostic(d,&r,kind)){
        if(!aic8800_transport_consume_receive(t,d))fault(d);else increment(&d->retired_records);
    }
out:d->entered=0;
}
int aic8800_dispatch_take_event(struct aic8800_dispatch *d,struct aic8800_dispatch_event *out)
{
    if(!d||!d->initialized||d->entered||!d->event_count||!independent(d,out,sizeof(*out)))return 0;
    *out=d->events[d->event_head];memset(&d->events[d->event_head],0,sizeof(*out));
    d->event_head=(d->event_head+1)%AIC8800_DISPATCH_EVENTS;--d->event_count;
    if(d->faulted){struct aic8800_sdio_response terminal;
        if(aic8800_transport_peek(d->transport,&terminal))(void)event(d,&terminal);}
    return 1;
}
int aic8800_dispatch_take_record(struct aic8800_dispatch *d,struct aic8800_dispatch_record *out)
{
    if(!d||!d->initialized||d->entered||!d->record_count||!independent(d,out,sizeof(*out)))return 0;
    *out=d->records[d->record_head];memset(&d->records[d->record_head],0,sizeof(*out));
    d->record_head=(d->record_head+1)%AIC8800_DISPATCH_RECORDS;--d->record_count;return 1;
}
int aic8800_dispatch_peek_data(const struct aic8800_dispatch *d,struct aic8800_data_frame *out)
{return d&&d->initialized&&!d->entered&&!d->faulted&&independent(d,out,sizeof(*out))&&
    aic8800_transport_peek_data(d->transport,out)&&!out->eapol;}
int aic8800_dispatch_consume_data(struct aic8800_dispatch *d)
{
    struct aic8800_data_frame frame;
    if(!aic8800_dispatch_peek_data(d,&frame))return 0;
    if(!aic8800_transport_consume_receive(d->transport,d)){fault(d);return 0;}return 1;
}
