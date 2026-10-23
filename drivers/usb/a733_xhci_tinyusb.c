/* SPDX-License-Identifier: GPL-2.0-only */
#include "a733_usb2_native.h"
#include "a733_range.h"
#include "tusb.h"
#include "host/hcd.h"
#include <string.h>
static struct a733_usb2_native *host;
static unsigned descriptor_index(unsigned address);
static struct a733_usb2_ss *ss_state(unsigned address)
{
    if(!host)return NULL;
    unsigned i=descriptor_index(address);
    return i<A733_XHCI_DEVICES?&host->descriptors[i].ss:NULL;
}

static void mounted_update(void)
{
    host->mounted=host->dual_leaf?(host->msc_mounted&&host->hid_mounted):
        (host->msc_mounted||host->hid_mounted||host->audio_mounted);
}
/* Hub state bound to a device record, to a logical address, or to the record
 * that owns a parent slot. */
static struct a733_usb2_hub *hub_record(unsigned record)
{
    for(unsigned i=0;i<A733_USB2_HUBS;++i)if(host->hubs[i].present&&host->hubs[i].record==record+1)return &host->hubs[i];
    return NULL;
}
static struct a733_usb2_hub *hub_at(unsigned address)
{
    for(unsigned i=0;address&&i<A733_USB2_HUBS;++i)if(host->hubs[i].present&&host->hubs[i].address==address)return &host->hubs[i];
    return NULL;
}
static struct a733_usb2_hub *parent_hub(unsigned record)
{
    unsigned slot=record<A733_XHCI_DEVICES?host->core.devices[record].parent:0;
    for(unsigned i=0;slot&&i<A733_XHCI_DEVICES;++i)if(host->core.devices[i].slot==slot)return hub_record(i);
    return NULL;
}
/* USB 2.0 section 7.2.1: a root or self-powered hub port supplies five unit
 * loads (500 mA), a bus-powered hub port one. The root is also bounded by the
 * qualified Type-C source. */
static unsigned upstream_ma(unsigned record)
{
    const struct a733_usb2_hub *h=parent_hub(record);
    unsigned source=host->platform.typec->config.source_ma;
    if(!host->core.devices[record].parent)return source<500?source:500;
    return !h?0:h->bus_powered?100:500;
}
static uint32_t le32(const uint8_t *p)
{return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static int owner(void)
{return host&&host->self==host&&host->core_started&&!host->failed&&host->core.ops.current(host->core.context)==1;}
static int clock_now(uint64_t *v)
{
    return owner()&&host->core.ops.now(host->core.context,v)==1&&
        a733_deadline_admit(*v,&host->last_now,host->platform.config.lifetime_deadline);
}
static uint64_t deadline(void)
{
    uint64_t now,later;if(!clock_now(&now)||!a733_deadline_after(now,1000000,&later))return 0;
    uint64_t d=host->hid.pending?host->hid.deadline:later;
    if(!host->mounted&&host->mount_deadline&&d>host->mount_deadline)d=host->mount_deadline;
    if(host->read_pending&&d>host->read_deadline)d=host->read_deadline;
    if(d>host->platform.config.lifetime_deadline)d=host->platform.config.lifetime_deadline;
    return d;
}
static void lost(void)
{
    if(!host)return;
    /* Direct core retention also serves the host fixture without native MMIO. */
    a733_xhci_lost(&host->core);host->failed=1;host->read_failed=1;host->mounted=0;
    a733_usb2_platform_lost(&host->platform);
}
/* A reported removal waits for the next attachment until the lifetime ends.
 * That attachment must mount within the startup budget measured from it. */
static void attaching(void)
{
    uint64_t now,end=host->platform.config.lifetime_deadline;
    if(host->mount_deadline)return;if(!clock_now(&now)){lost();return;}
    host->mount_deadline=end-now>host->mount_budget?now+host->mount_budget:end;
}
static uint64_t mount_bound(void)
{return host->mount_deadline?host->mount_deadline:host->platform.config.lifetime_deadline;}
/* A removed leaf ends its outstanding request without a result or copy. The
 * core has revoked, or is about to revoke, the request's DMA destination. */
static void withdraw_msc(void)
{host->msc_mounted=0;host->read_pending=host->read_complete=0;host->read_failed=1;}
static void withdraw_hid(void)
{
    struct a733_usb2_hid *h=&host->hid;host->hid_mounted=0;
    h->pending=h->complete=h->event_seen=h->event_bytes=h->requested=0;h->pending_buffer=NULL;
}
static void withdraw_audio(void)
{
    struct a733_usb2_audio *a=&host->audio;host->audio_mounted=0;
    a->starting=a->started=0;a->pending_buffer=NULL;a->pending_bytes=0;
}
static void leaf_kind(void)
{
    host->kind=host->msc_seen&&host->hid_seen?A733_USB2_DEVICE_DUAL:host->msc_seen?A733_USB2_DEVICE_MSC:
        host->hid_seen?A733_USB2_DEVICE_HID:host->audio_seen?A733_USB2_DEVICE_AUDIO:A733_USB2_DEVICE_NONE;
    mounted_update();
}
/* Clear the adapter state that names a device whose slot teardown started.
 * Descriptor state follows core records that are detaching or unbound; the
 * core clears a record itself only after its Disable Slot completion. Address
 * 0 names no leaf or hub identity, so it clears only descriptor state. */
static void forget(unsigned address)
{
    struct a733_usb2_native *n=host;
    for(unsigned i=0;i<A733_XHCI_DEVICES;++i)
        if(!n->core.devices[i].port_ready||n->core.devices[i].detach)memset(&n->descriptors[i],0,sizeof(n->descriptors[i]));
    for(unsigned i=0;i<A733_USB2_HUBS;++i){
        struct a733_usb2_hub *h=&n->hubs[i];const struct a733_xhci_device *d=h->record?&n->core.devices[h->record-1]:NULL;
        if(h->present&&(!d||!d->port_ready||d->detach))memset(h,0,sizeof(*h));
    }
    if(n->control_buffer&&n->control_address==address){n->control_buffer=NULL;n->control_bytes=0;}
    if(!address)return;
    if(n->msc_seen&&address==n->address){
        withdraw_msc();n->msc_seen=n->msc_in=n->msc_out=n->blocks=n->block_bytes=0;
        n->bot_tag=n->bot_total=n->bot_received=0;n->bot_buffer=NULL;
        n->bot_phase=n->bot_opcode=n->bot_stage=n->bot_endpoint=n->bot_requested=n->bot_status=0;
    }
    if(n->hid_seen&&address==n->hid_address){
        /* Report sequence numbers stay monotonic across devices. */
        uint64_t sequence=n->hid.sequence;memset(&n->hid,0,sizeof(n->hid));n->hid.sequence=sequence;
        n->hid_seen=n->hid_mounted=n->hid_address=0;
    }
    if(n->audio_seen&&address==n->audio.address){
        uint64_t sequence=n->audio.sequence;memset(&n->audio,0,sizeof(n->audio));n->audio.sequence=sequence;
        n->audio_seen=n->audio_mounted=0;
    }
    if(address==n->address)n->address=0;
    leaf_kind();
}
uint32_t tusb_time_millis_api(void)
{uint64_t now;if(!clock_now(&now)){lost();return host?(uint32_t)(host->last_now/1000):0;}return (uint32_t)(now/1000);}
int a733_usb2_host_bind(struct a733_usb2_native *n)
{
    if(host||!n||n->self!=n||!n->core_started||!n->core.self||n->core.ops.current(n->core.context)!=1)return 0;
    host=n;uint64_t now;if(!clock_now(&now)||now>=n->platform.config.deadline){lost();return 0;}
    n->mount_deadline=n->platform.config.deadline;n->mount_budget=n->platform.config.deadline-now;
    const tusb_rhport_init_t init={.role=TUSB_ROLE_HOST,.speed=n->core.require_ss?TUSB_SPEED_SUPER:TUSB_SPEED_HIGH};
    if(!tusb_init(0,&init)){lost();return 0;}return 1;
}
int a733_usb2_host_poll(struct a733_usb2_native *n)
{
    uint64_t now;if(n!=host||!clock_now(&now)){lost();return -1;}
    if((!host->mounted&&host->mount_deadline&&now>=host->mount_deadline)||
        (host->read_pending&&now>=host->read_deadline)||
        (host->hid.pending&&now>=host->hid.deadline)){lost();return -1;}
    if(host->audio.starting&&now>=host->audio.start_deadline){lost();return -1;}
    for(unsigned i=0;i<A733_USB2_HUBS;++i){
        struct a733_usb2_hub *h=&host->hubs[i];
        if(h->present&&h->status_deferred&&now>=h->power_good_deadline&&host->core.devices[h->record-1].hub_configured){
            if(!a733_xhci_transfer(&host->core,h->address,h->endpoint,h->status_buffer,h->status_bytes,
               host->platform.config.lifetime_deadline)){
                lost();return -1;
            }
            h->status_deferred=0;
        }
    }
    enum a733_xhci_result r=a733_xhci_poll(&host->core);
    if(r==A733_XHCI_RETAINED||r==A733_XHCI_INVALID){lost();return -1;}
    /* Notice validation records metadata only. Apply it after core callbacks
     * return, before the queued TinyUSB completion can issue another request. */
    for(unsigned i=0;i<A733_USB2_HUBS;++i){
        struct a733_usb2_hub *h=&host->hubs[i];
        if(!host->failed&&h->mark_pending){
            if(!a733_xhci_mark_hub(&host->core,h->mark_address))lost();
            h->mark_pending=0;
        }
        if(!host->failed&&h->configure_pending){
            if(!a733_xhci_hub_configure(&host->core,h->address,h->ports,h->ttt,h->alt_selected))lost();
            h->configure_pending=0;
        }
    }
    if(!host->failed&&r!=A733_XHCI_BUSY)tuh_task_ext(0,false);
#if CFG_TUH_AUDIO
    /* Hand completed isochronous packets to the pending class request. Each
     * completion lets the class queue its next request in the same poll. */
    struct a733_usb2_audio *a=&host->audio;
    for(unsigned k=0;k<A733_XHCI_ISOCH_TDS&&!host->failed&&a->pending_buffer;++k){
        unsigned bytes=0;int got=a733_xhci_isoch_take(&host->core,a->address,a->endpoint,a->pending_buffer,a->pending_bytes,&bytes);
        if(got<0){lost();break;}
        if(!got)break;
        a->pending_buffer=NULL;a->pending_bytes=0;
        hcd_event_xfer_complete((uint8_t)a->address,(uint8_t)a->endpoint,bytes,XFER_RESULT_SUCCESS,false);
        tuh_task_ext(0,false);
    }
#endif
    if(host->failed)a733_xhci_lost(&host->core);
    return host->failed?-1:host->mounted?1:0;
}
/* BOT command/data/status ownership is checked before the TinyUSB class sees
 * an event. Its pinned MSC implementation does not itself verify CSW identity.
 * Only read-only SCSI commands needed by enumeration and READ10 are admitted. */
static int bot_submit(unsigned ep,uint8_t *buffer,unsigned bytes)
{
    struct a733_usb2_native *n=host;if(n->bot_stage)return 0;
    if(!n->bot_phase){
        if((ep&128)||bytes!=31||!buffer||le32(buffer)!=UINT32_C(0x43425355)||buffer[13]||
            (buffer[12]&127)||buffer[14]>16||!buffer[14])return 0;
        unsigned op=buffer[15],length=le32(buffer+8),cdb=buffer[14];
        if((length&&buffer[12]!=128)||length>A733_XHCI_TRANSFER_BYTES)return 0;
        if(!((op==0&&length==0&&cdb==6)||(op==3&&length==18&&cdb==6)||
            (op==0x12&&length==36&&cdb==6)||(op==0x25&&length==8&&cdb==10)||
            (op==0x28&&length==512&&cdb==10&&n->read_pending)))return 0;
        n->bot_tag=le32(buffer+4);n->bot_total=length;n->bot_received=0;n->bot_opcode=op;n->bot_stage=1;
    }else if(n->bot_phase==1){if(!(ep&128)||bytes!=n->bot_total||!buffer)return 0;n->bot_stage=2;}
    else if(n->bot_phase==2){if(!(ep&128)||bytes!=13||!buffer)return 0;n->bot_stage=3;}
    else return 0;
    n->bot_endpoint=ep;n->bot_requested=bytes;n->bot_buffer=buffer;return 1;
}
static int bot_complete(const struct a733_xhci_notice *e)
{
    struct a733_usb2_native *n=host;
    if(!n->bot_stage||e->endpoint!=n->bot_endpoint||e->result||e->bytes>n->bot_requested)return 0;
    if(n->bot_stage==1){if(e->bytes!=31)return 0;n->bot_phase=n->bot_total?1:2;}
    else if(n->bot_stage==2){n->bot_received=e->bytes;n->bot_phase=2;}
    else {
        const uint8_t *p=n->bot_buffer;
        if(e->bytes!=13||le32(p)!=UINT32_C(0x53425355)||le32(p+4)!=n->bot_tag||
            le32(p+8)!=n->bot_total-n->bot_received||p[12]>1)return 0;
        if(!p[12]&&n->bot_received!=n->bot_total)return 0;
        n->bot_status=p[12];n->bot_phase=0;
    }
    n->bot_stage=0;n->bot_buffer=NULL;return 1;
}
static unsigned le16(const uint8_t *p){return p[0]|(unsigned)p[1]<<8;}
static uint32_t le24(const uint8_t *p){return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16;}
static int ss_bos(struct a733_usb2_ss *ssdata,const uint8_t *p,unsigned bytes)
{
    if(bytes<5||p[0]!=5||p[1]!=15||le16(p+2)<15||le16(p+2)>sizeof(ssdata->ss_bos)||!p[4])return 0;
    if(bytes==5){if(ssdata->ss_control)return 0;memcpy(ssdata->ss_bos_prefix,p,5);ssdata->ss_bos_bytes=le16(p+2);ssdata->ss_control=1;return 1;}
    if(ssdata->ss_control!=1||bytes!=ssdata->ss_bos_bytes||memcmp(ssdata->ss_bos_prefix,p,5))return 0;
    unsigned count=0,seen=0;
    for(unsigned at=5;at<bytes;){
        if(bytes-at<3||p[at]<3||p[at]>bytes-at||p[at+1]!=16)return 0;
        const uint8_t *c=p+at;
        if(c[2]==3){
            if(seen++||c[0]!=10||(c[3]&~2u)||!(le16(c+4)&8)||le16(c+4)&~15u||
               c[6]>3||!(le16(c+4)&(1u<<c[6]))||c[7]>10||le16(c+8)>2047)return 0;
            ssdata->ss_u1=c[7];ssdata->ss_u2=le16(c+8);
        }else if(c[2]==2){if(c[0]!=7||(le32(c+3)&1))return 0;}
        else if(c[2]==4&&c[0]!=20)return 0;
        ++count;at+=c[0];
    }
    if(seen!=1||count!=p[4])return 0;
    memcpy(ssdata->ss_bos,p,bytes);ssdata->ss_control=2;return 1;
}
bool a733_tinyusb_ss_direct(uint8_t address)
{
    unsigned i=descriptor_index(address);
    return owner()&&i<A733_XHCI_DEVICES&&!host->core.devices[i].parent;
}
bool a733_tinyusb_ss_enumerate(uint8_t address,unsigned stage,uint8_t *buffer,tuh_xfer_cb_t callback,uintptr_t next)
{
    struct a733_usb2_ss *ssdata=ss_state(address);
    if(!ssdata||!owner()||a733_xhci_device_speed(&host->core,address)!=4||!address||!buffer||stage!=ssdata->ss_control||stage>3||
       host->core.ops.outside(host->core.context,buffer,256)!=1){lost();return false;}
    tusb_control_request_t request={0};request.bRequest=stage<2?6:stage==2?49:48;
    request.bmRequestType=stage<2?0x80:0;const struct a733_usb2_hub *parent=parent_hub(descriptor_index(address));
    request.wValue=stage<2?0x0f00:stage==2?(parent?80+parent->delay_ns:40):0;
    request.wLength=stage==0?5:stage==1?(uint16_t)ssdata->ss_bos_bytes:stage==3?6:0;
    if(stage==3){
        unsigned u1=ssdata->ss_u1>host->core.root_u1?ssdata->ss_u1:host->core.root_u1;
        unsigned u2=ssdata->ss_u2>host->core.root_u2?ssdata->ss_u2:host->core.root_u2;
        if(!u1)u1=1;if(!u2)u2=1;
        ssdata->ss_sel[0]=ssdata->ss_sel[1]=(uint8_t)u1;
        ssdata->ss_sel[2]=ssdata->ss_sel[4]=(uint8_t)u2;ssdata->ss_sel[3]=ssdata->ss_sel[5]=(uint8_t)(u2>>8);
        memcpy(buffer,ssdata->ss_sel,6);
    }
    ssdata->ss_request=stage;
    tuh_xfer_t xfer={.daddr=address,.ep_addr=0,.setup=&request,.buffer=request.wLength?buffer:NULL,.complete_cb=callback,.user_data=next};
    return tuh_control_xfer(&xfer);
}
/* One USB Audio Class 1 function: an Audio Control interface (optionally with
 * its interrupt endpoint) followed by Audio Streaming interfaces whose alternate
 * 0 has no endpoint. Every capture alternate setting uses the same isochronous
 * IN data endpoint; playback interfaces may exist and are never opened. Only
 * capture alternates with at most A733_XHCI_ISOCH_PACKET bytes per packet and
 * a 1 ms or shorter service interval are recorded. TinyUSB's audio class
 * parses the class-specific descriptors themselves; only the Feature Unit IDs
 * of the Audio Control interface are recorded here, for its control requests. */
static int audio_configuration(unsigned address,const uint8_t *p,unsigned total,unsigned speed)
{
    struct a733_usb2_audio a={0};
    unsigned interfaces=0,control=0,current=0,alternate=0,subclass=0,expected=0,seen=0,open=0;
    for(unsigned at=9;at<total;){
        unsigned length=p[at],type=at+1<total?p[at+1]:0;
        if(length<2||length>total-at)return 0;
        if(type==4){
            if(length!=9||(open&&seen!=expected))return 0;
            current=p[at+2];alternate=p[at+3];expected=p[at+4];subclass=p[at+6];seen=0;open=1;
            if(p[at+5]!=1||p[at+7]||current>=32)return 0;
            if(subclass==1){
                if(control||alternate||expected>1)return 0;
                control=1;a.control_interface=current;++interfaces;
            }else if(subclass==2){
                if(!control||current==a.control_interface)return 0;
                if(!alternate){if(expected||(a.streaming&(1u<<current)))return 0;a.streaming|=1u<<current;++interfaces;}
                else if(!(a.streaming&(1u<<current))||!expected||expected>2)return 0;
            }else return 0;
        }else if(type==5){
            if((length!=7&&length!=9)||!open||++seen>expected)return 0;
            unsigned ep=p[at+2],attributes=p[at+3],size=le16(p+at+4),interval=p[at+6];
            if(subclass==1){if((attributes&3)!=3||!(ep&128))return 0;}
            else if((attributes&3)!=1||(ep&0x70)||!(ep&15))return 0;
            /* UAC1 marks a data endpoint by a synchronization type; an IN
             * endpoint without one is a playback feedback endpoint. */
            else if((ep&128)&&(attributes&12)){
                if(a.endpoint&&(a.endpoint!=ep||a.interface!=current))return 0;
                a.endpoint=ep;a.interface=current;
                if(!(size&~0x7ffu)&&size&&size<=A733_XHCI_ISOCH_PACKET&&interval&&interval<=(speed==3?4u:1u)){
                    if(a.alternates==A733_USB2_AUDIO_ALTERNATES)return 0;
                    a.alternate[a.alternates++]=(struct a733_usb2_audio_alternate){current,alternate,size,interval};
                }
            }
        }else if(type==0x24&&open&&subclass==1&&length>=7&&p[at+2]==6&&p[at+3])
            a.features[p[at+3]>>5]|=UINT32_C(1)<<(p[at+3]&31);
        else if(type!=0x24&&type!=0x25)return 0;
        at+=length;
    }
    if(!control||(open&&seen!=expected)||interfaces!=p[4]||!a.alternates)return 0;
    a.address=address;host->audio=a;return 1;
}
static int control_response(const struct a733_xhci_notice *e)
{
    struct a733_usb2_native *n=host;
    if(n->control_device>=A733_XHCI_DEVICES||e->address!=n->control_address)return 0;
    struct a733_usb2_descriptor *desc=&n->descriptors[n->control_device];
    struct a733_xhci_device *device=&n->core.devices[n->control_device];
    struct a733_usb2_hub *hub=hub_record(n->control_device);
    struct a733_usb2_ss *ssdata=&desc->ss;
    const uint8_t *p=n->control_buffer,*s=n->control_setup;unsigned bytes=n->control_bytes;
    if(e->result==1){
        /* TinyUSB invokes the class fallback immediately on STALLED, without
         * hcd_edpt_clear_stall. The core has already completed Reset Endpoint
         * and Set TR Dequeue; the old stage/caller can now be forgotten. */
        static const uint8_t max_lun[8]={0xa1,0xfe,0,0,0,0,1,0};
        if((e->endpoint&15)||e->bytes||desc->kind!=A733_USB2_DEVICE_MSC||
           !n->msc_seen||e->address!=n->address||memcmp(s,max_lun,8)||
           device->control_stage||device->endpoints[0].pending||
           device->endpoints[0].pending_trb||device->endpoints[0].caller)return 0;
        n->control_buffer=NULL;n->control_bytes=0;return 1;
    }
    if(!p){
        if(device->speed==4&&device->control_stage==0&&e->bytes==0&&e->endpoint==128){
            if(s[1]==49){if(ssdata->ss_control!=2||ssdata->ss_request!=2)return 0;ssdata->ss_control=3;if(device->parent)ssdata->ss_ready=1;}
            if(s[1]==48){if(ssdata->ss_control!=3||ssdata->ss_request!=3)return 0;ssdata->ss_control=4;ssdata->ss_ready=1;}
        }
        if(device->control_stage==0&&e->bytes==0&&e->endpoint==128&&desc->kind==A733_USB2_DEVICE_HUB&&hub){
            uint64_t now;
            if(s[0]==1&&s[1]==11&&le16(s+2)==1)hub->alt_selected=1;
            if(s[0]==0x20&&s[1]==12&&device->speed==4)hub->depth_set=1;
            if(s[0]==0x23&&s[1]==3&&device->speed==4){
                if(le16(s+2)==23)hub->u1_disabled|=1u<<le16(s+4);
                if(le16(s+2)==24)hub->u2_disabled|=1u<<le16(s+4);
            }
            if(s[0]==0x23&&s[1]==3&&le16(s+2)==8){
                if(device->speed==4&&(!hub->depth_set||!(hub->u1_disabled&(1u<<le16(s+4)))||
                   !(hub->u2_disabled&(1u<<le16(s+4)))))return 0;
                unsigned port=le16(s+4);if(!port||port>hub->ports||!clock_now(&now))return 0;
                hub->powered_mask|=1u<<port;
                if(hub->powered_mask==((2u<<hub->ports)-2)){
                    uint64_t delay=(uint64_t)hub->power_good_2ms*2000;
                    if(!a733_deadline_after(now,delay,&hub->power_good_deadline))return 0;
                }
            }
        }
        return 1;
    }
    if(e->endpoint!=128||e->result||e->bytes!=bytes||bytes!=le16(s+6))return 0;
    n->control_buffer=NULL;n->control_bytes=0;
    if(device->speed==4&&s[0]==0x80&&s[1]==6&&s[3]==15)return ss_bos(ssdata,p,bytes);
    if(desc->kind==A733_USB2_DEVICE_HUB){
        if(!hub)return 0;
        if(s[0]==0xa0&&s[1]==6&&le16(s+2)==0x2a00){
            unsigned chars=bytes>=5?le16(p+3):0,rem=bytes==12?le16(p+10):0;
            if(device->speed!=4||bytes!=12||p[0]!=12||p[1]!=0x2a||!p[2]||p[2]>7||
               chars&~31u||(chars&3)>1||((chars>>3)&3)>1||p[7]>10||le16(p+8)>400||
               rem&~((2u<<p[2])-2))return 0;
            hub->ports=p[2];hub->ttt=0;hub->power_good_2ms=p[5];
            hub->hdr_latency=p[7];hub->delay_ns=le16(p+8);hub->configure_pending=1;return 1;
        }
        if(s[0]==0xa0&&s[1]==6&&le16(s+2)==0x2900){
            unsigned chars=bytes>=5?le16(p+3):0;
            if(bytes!=9||p[0]!=9||p[1]!=0x29||!p[2]||p[2]>7||chars&~255u||
               (chars&3)>1||((chars>>3)&3)>1||p[7]&1||p[7]&~((2u<<p[2])-2)||p[8]!=255)return 0;
            if(hub->protocol==2&&!hub->alt_selected)return 0;
            /* A bus-powered hub's configuration current excludes its ports,
             * each of which may draw one unit load (USB 2.0 section 11.13). */
            if(hub->bus_powered&&hub->max_power_ma+100u*p[2]>upstream_ma(n->control_device))return 0;
            hub->ports=p[2];hub->ttt=(chars>>5)&3;hub->power_good_2ms=p[5];
            hub->configure_pending=1;return 1;
        }
        if(s[1]==0&&s[0]==0x80){
            if(bytes!=2||(le16(p)&~3u)||(p[0]&1)==hub->bus_powered)return 0;hub->device_power=1;return 1;
        }
        if(s[1]==0&&s[0]==0xa0){
            /* Local Power Source describes a local supply, which a
             * bus-powered hub does not have; over-current must be clear. */
            if(bytes!=4||!hub->device_power||(p[0]&~(hub->bus_powered?1u:0u))||p[1]||p[3]||(p[2]&~1u))return 0;
            hub->local_power=1;return 1;
        }
        if(s[1]==0&&s[0]==0xa3){
            unsigned port=le16(s+4),status=bytes==4?le16(p):0,changes=bytes==4?le16(p+2):0;
            if(device->speed==4){
                if(bytes!=4||!port||port>hub->ports||!hub->depth_set||status&~0x3f3u||
                   changes&~17u||!(status&512u)||
                   ((status&2u)?(status&0x1e0u):((status&0x1e0u)!=0&&(status&0x1e0u)!=160&&(status&0x1e0u)!=224&&(status&0x1e0u)!=288))||
                   (status&2u&&status&16u))return 0;
            }else if(bytes!=4||!port||port>hub->ports||status&~0x1f1fu||changes&~31u||
               status&(8u|4u|2048u)||changes&8u||!(status&256u)||
               (status&1536u)==1536u)return 0;
            if(status&1){
                if(!(hub->connected_mask&(1u<<port))){
                    unsigned mask=hub->connected_mask;
                    if(mask&&(!n->dual_leaf||(mask&(mask-1))))return 0;
                    hub->connected_mask|=1u<<port;hub->selected_port=port;attaching();
                }
            }else if(hub->connected_mask&(1u<<port)){
                /* Only a reported disconnect of a disabled port removes the
                 * child: TinyUSB then closes it and the core disables its slot. */
                if(!(changes&1)||(status&2))return 0;
                hub->connected_mask&=~(1u<<port);n->mount_deadline=0;return 1;
            }
            for(unsigned j=1;j<A733_XHCI_DEVICES;++j)
                if(n->core.devices[j].parent==device->slot&&n->core.devices[j].parent_port==port&&n->core.devices[j].zero_retired&&
                   !n->core.devices[j].detach&&(!(status&2)||(changes&2)))return 0;
            return 1;
        }
    }
    if(s[0]==0xa1&&s[1]==0xfe)return desc->kind==A733_USB2_DEVICE_MSC&&bytes==1&&p[0]==0;
    /* UAC1 Feature Unit GET_CUR/MIN/MAX/RES values; hcd_setup_send bounded
     * the request and the length matched it exactly. */
    if(s[0]==0xa1&&s[1]>=0x81&&s[1]<=0x84)return desc->kind==A733_USB2_DEVICE_AUDIO;
    if(s[0]==0x81&&s[1]==6&&s[3]==0x22)return desc->kind==A733_USB2_DEVICE_HID&&bytes==n->hid.descriptor_bytes;
    if(s[0]!=0x80||s[1]!=6||bytes<2||p[1]!=s[3])return 0;
    if(s[3]==1){
        unsigned speed=device->speed,is_hub=p[4]==9;
        /* A second hub tier is a USB2 high-speed hub on a port of the root-port
         * hub, admitted only when the session selects two tiers. */
        unsigned nested=n->control_device&&n->hub_tiers>=2&&speed==3&&device->tier==1&&(p[6]==1||p[6]==2);
        if(!n->control_device&&n->platform.config.topology_mode&&
           is_hub!=(n->platform.config.topology_mode==2))return 0;
        if(speed==4&&(p[2]||p[3]!=3||p[7]!=9))return 0;
        if(p[0]!=18||p[5]||(!is_hub&&(p[4]||p[6]))||
           (is_hub&&!nested&&(n->control_device||(speed==4?p[6]!=3:(speed!=3||(p[6]!=1&&p[6]!=2)))))||
           (speed==2&&p[7]!=8)||(speed==3&&p[7]!=64)||
           (speed==1&&p[7]!=8&&p[7]!=16&&p[7]!=32&&p[7]!=64))return 0;
        if(bytes==8){
            if(desc->device_probe)return 0;memcpy(desc->device_prefix,p,8);desc->device_probe=1;
            if(is_hub){
                for(unsigned i=0;!hub&&i<A733_USB2_HUBS;++i)if(!n->hubs[i].present)hub=&n->hubs[i];
                if(!hub)return 0;
                *hub=(struct a733_usb2_hub){.present=1,.record=n->control_device+1,.protocol=p[6],
                    .mark_pending=1,.mark_address=e->address};
                desc->kind=A733_USB2_DEVICE_HUB;
            }
            return 1;
        }
        if(bytes!=18||!desc->device_probe||memcmp(desc->device_prefix,p,8)||p[17]!=1)return 0;
        if(is_hub){if(!hub)return 0;hub->address=e->address;}
        return 1;
    }
    if(s[3]==2){
        unsigned total=bytes>=4?le16(p+2):0,is_hub=desc->kind==A733_USB2_DEVICE_HUB,ss=device->speed==4;
        /* Self power (bmAttributes 0xc0), or bus power (0x80) when admitted. */
        unsigned powered=(p[7]&0xc0)==0xc0||(n->bus_powered_hubs&&!ss&&(p[7]&0xc0)==0x80);
        unsigned audio=CFG_TUH_AUDIO&&n->audio_capture&&!is_hub&&(device->speed==1||device->speed==3)&&
            bytes>=9&&p[4]>=2&&p[4]<=4&&total>9&&total<=CFG_TUH_ENUMERATION_BUFSIZE;
        if(ss&&(!ssdata->ss_ready||(p[8]*8u>900u||p[8]*8u>n->platform.typec->config.source_ma)))return 0;
        if(!ss&&p[8]*2u>upstream_ma(n->control_device))return 0;
        if(is_hub&&!hub)return 0;
        if(bytes<9||p[0]!=9||(!audio&&p[4]!=1)||p[5]!=1||!(p[7]&128)||p[8]>250||
           (is_hub?(total!=(ss?31u:hub->protocol==2?41u:25u)||!powered||(p[7]&31)):
            audio?0:(ss?(total!=40&&total!=44):(total!=32&&total!=34))))return 0;
        if(bytes==9){if(desc->configuration_probe)return 0;memcpy(desc->configuration_prefix,p,9);desc->configuration_probe=1;return 1;}
        if(bytes!=total||!desc->configuration_probe||memcmp(desc->configuration_prefix,p,9)||p[9]!=9||p[10]!=4||p[11]||p[12])return 0;
        if(audio){
            if(n->audio_seen||n->msc_seen||n->hid_seen||!audio_configuration(e->address,p,total,device->speed))return 0;
            n->audio_seen=1;desc->kind=A733_USB2_DEVICE_AUDIO;n->kind=A733_USB2_DEVICE_AUDIO;return 1;
        }
        if(is_hub){
            hub->bus_powered=!(p[7]&0x40);hub->max_power_ma=p[8]*2u;
            for(unsigned alt=0;alt<(hub->protocol==2?2u:1u);++alt){
                unsigned at=9+16*alt;const uint8_t *q=p+at;
                if(q[0]!=9||q[1]!=4||q[2]||q[3]!=alt||q[4]!=1||q[5]!=9||q[6]||q[7]!=(ss?0u:alt+1)||
                   q[9]!=7||q[10]!=5||q[11]&~0x8fu||!(q[11]&128)||!(q[11]&15)||q[12]!=3||
                   !le16(q+13)||le16(q+13)>64||!q[15]||q[15]>16)return 0;
                if(!alt){hub->endpoint=q[11];hub->packet=le16(q+13);hub->interval=q[15];}
                else if(q[11]!=hub->endpoint||le16(q+13)!=hub->packet||q[15]!=hub->interval)return 0;
            }
            if(ss&&(p[25]!=6||p[26]!=0x30||p[27]||p[28]||le16(p+29)!=hub->packet))return 0;
            return 1;
        }
        if(bytes==(ss?40u:34u)){
            unsigned ep=p[29],packet=le16(p+31),interval=p[33];
            if(p[13]!=1||p[14]!=3||p[15]>1||p[16]>2||(!p[15]&&p[16])||
               (p[15]&&!p[16])||p[18]!=9||p[19]!=0x21||p[23]!=1||p[24]!=0x22||
               !le16(p+25)||le16(p+25)>256||p[27]!=7||p[28]!=5||
               ep&~0x8fu||!(ep&128)||!(ep&15)||p[30]!=3||!packet||
               packet>(device->speed==2?8u:64u)||!interval||interval>(device->speed>=3?16u:255u))return 0;
            if(ss&&(p[15]!=1||p[16]!=1||p[34]!=6||p[35]!=0x30||p[36]||p[37]||le16(p+38)!=packet))return 0;
            if(n->hid_seen||n->audio_seen||(n->msc_seen&&!n->dual_leaf))return 0;
            n->hid_seen=1;n->hid_address=e->address;
            desc->kind=A733_USB2_DEVICE_HID;n->kind=n->msc_seen?A733_USB2_DEVICE_DUAL:A733_USB2_DEVICE_HID;n->hid.endpoint=ep;n->hid.packet=packet;n->hid.interval=interval;
            n->hid.subclass=p[15];n->hid.protocol=p[16];n->hid.descriptor_bytes=le16(p+25);return 1;
        }
        if((device->speed!=3&&device->speed!=4)||p[13]!=2||p[14]!=8||p[15]!=6||p[16]!=0x50)return 0;
        unsigned dirs=0,seen=0;
        for(unsigned at=18;at<total;at+=ss?13:7){unsigned ep=p[at+2];
            if(p[at]!=7||p[at+1]!=5||(ep&0x70)||!(ep&15)||p[at+3]!=2||le16(p+at+4)!=(ss?1024u:512u))return 0;
            if(ss){const uint8_t *c=p+at+7;if(c[0]!=6||c[1]!=0x30||c[2]>15||c[3]>16||le16(c+4))return 0;ssdata->ss_burst[!!(ep&128)]=c[2];}
            if(ep&128)n->msc_in=ep;else n->msc_out=ep;
            unsigned bit=1u<<((ep&15)+((ep&128)?16:0));if(seen&bit)return 0;seen|=bit;dirs|=(ep&128)?2:1;}
        if(dirs!=3||n->msc_seen||n->audio_seen||(n->hid_seen&&!n->dual_leaf))return 0;
        n->msc_seen=1;n->address=e->address;desc->kind=A733_USB2_DEVICE_MSC;
        n->kind=n->hid_seen?A733_USB2_DEVICE_DUAL:A733_USB2_DEVICE_MSC;return 1;
    }
    if(s[3]==3){
        if(p[0]<2||(p[0]&1)||(!s[2]&&p[0]<4))return 0;
        if(bytes==2){desc->string_bytes=p[0];desc->string_id=s[2];desc->string_language=le16(s+4);return 1;}
        return bytes==p[0]&&bytes==desc->string_bytes&&s[2]==desc->string_id&&le16(s+4)==desc->string_language&&
            (s[2]||bytes>=4);
    }
    return 0;
}
void a733_usb2_host_notice(void *cookie,const struct a733_xhci_notice *event)
{
    struct a733_usb2_native *n=cookie;
    if(n!=host||!event||event->generation!=host->core.generation){lost();return;}
    if(event->kind==A733_XHCI_ATTACH){
        /* The core announces a root attachment only with no slot bound, so no
         * device state from an earlier attachment can remain in use. */
        forget(0);if(n->msc_seen)forget(n->address);if(n->hid_seen)forget(n->hid_address);
        if(n->audio_seen)forget(n->audio.address);
        memset(n->hubs,0,sizeof(n->hubs));n->control_buffer=NULL;n->control_bytes=0;
        attaching();hcd_event_device_attach(0,false);
    }else if(event->kind==A733_XHCI_REMOVE){
        /* Result 0 is an orderly root disconnect: the core is tearing down
         * every slot and has revoked every caller destination. */
        if(event->result){lost();return;}
        withdraw_msc();withdraw_hid();withdraw_audio();n->control_buffer=NULL;n->control_bytes=0;
        mounted_update();n->mount_deadline=0;hcd_event_device_remove(0,false);
    }
    else if(event->kind==A733_XHCI_TRANSFER){
        uint64_t now,d=deadline();
        if(!d||!clock_now(&now)||host->core.ops.live(host->core.context,host->core.generation,d)!=1){lost();return;}
        if(event->endpoint&15){
            struct a733_usb2_hub *h=hub_at(event->address);
            if(h){
                if(event->endpoint!=h->endpoint||!h->status_requested||h->status_deferred||
                   !h->status_buffer||event->result||event->bytes!=1||
                   (h->status_buffer[0]&~((2u<<h->ports)-1))){lost();return;}
                h->status_requested=0;h->status_buffer=NULL;
            }else if(n->hid_seen&&event->address==n->hid_address){
                if(!n->hid.pending||n->hid.event_seen||event->endpoint!=n->hid.endpoint||
                   event->address!=n->hid_address||event->result||event->bytes>n->hid.requested){lost();return;}
                n->hid.event_seen=1;n->hid.event_bytes=event->bytes;
            }else if(!n->msc_seen||event->address!=n->address||!bot_complete(event)){lost();return;}
        }else if(!control_response(event)){lost();return;}
        hcd_event_xfer_complete((uint8_t)event->address,(uint8_t)event->endpoint,event->bytes,
            event->result==0?XFER_RESULT_SUCCESS:event->result==1?XFER_RESULT_STALLED:XFER_RESULT_FAILED,false);
    }else lost();
}
bool hcd_init(uint8_t port,const tusb_rhport_init_t *i)
{return owner()&&!port&&i&&i->role==TUSB_ROLE_HOST&&i->speed==(host->core.require_ss?TUSB_SPEED_SUPER:TUSB_SPEED_HIGH);}
bool hcd_deinit(uint8_t port){(void)port;lost();return false;}
bool hcd_configure(uint8_t port,uint32_t id,const void *p){(void)port;(void)id;(void)p;return false;}
void hcd_int_enable(uint8_t port){(void)port;}
void hcd_int_disable(uint8_t port){(void)port;}
void hcd_int_handler(uint8_t port,bool isr){(void)port;(void)isr;lost();}
uint32_t hcd_frame_number(uint8_t port)
{
    uint32_t v;uint64_t d=deadline();
    if(port||!d||!host->core.runtime||host->core.ops.live(host->core.context,host->core.generation,d)!=1||
        host->core.ops.read32(host->core.context,host->core.runtime,&v)!=1)return 0;
    return (v&0x3fff)>>3;
}
bool hcd_port_connect_status(uint8_t port){return owner()&&!port&&a733_xhci_port_connected(&host->core);}
void hcd_port_reset(uint8_t port)
{uint64_t d=deadline();if(port||!d||!a733_xhci_reset_port(&host->core,d))lost();}
void hcd_port_reset_end(uint8_t port){if(port||!owner())lost();}
tusb_speed_t hcd_port_speed_get(uint8_t port)
{return owner()&&!port?(host->core.speed==4?TUSB_SPEED_SUPER:host->core.speed==3?TUSB_SPEED_HIGH:host->core.speed==2?TUSB_SPEED_LOW:host->core.speed==1?TUSB_SPEED_FULL:TUSB_SPEED_INVALID):TUSB_SPEED_INVALID;}
/* Closing a hub child, or any device after an orderly root disconnect, uses
 * the core's slot teardown. Closing an attached root device has no port
 * change behind it, so the core refuses that detach and the session ends. */
void hcd_device_close(uint8_t port,uint8_t address)
{
    if(!owner()||port){lost();return;}
    if(!address&&a733_xhci_retire_zero(&host->core))return;
    if(!a733_xhci_detach(&host->core,address)){lost();return;}
    forget(address);
}

bool hcd_edpt_open(uint8_t port,uint8_t address,const tusb_desc_endpoint_t *e)
{
    uint64_t d=deadline();if(port||!e||!d)return false;
    if(!address&&host->hubs[0].present&&host->core.devices[0].zero_retired){
        tuh_bus_info_t bus;const struct a733_usb2_hub *parent;
        if(!tuh_bus_info_get(0,&bus)||bus.rhport||!(parent=hub_at(bus.hub_addr))||
           !bus.hub_port||bus.hub_port>7||!(parent->connected_mask&(1u<<bus.hub_port))||
           !a733_xhci_bind_child(&host->core,bus.hub_addr,bus.hub_port,
             bus.speed==TUSB_SPEED_SUPER?4:bus.speed==TUSB_SPEED_HIGH?3:bus.speed==TUSB_SPEED_LOW?2:bus.speed==TUSB_SPEED_FULL?1:0,d)){lost();return false;}
    }
    struct a733_usb2_ss *ssdata=ss_state(address);if(!ssdata)return false;
    unsigned speed=a733_xhci_device_speed(&host->core,address);
    if(e->bmAttributes.xfer==TUSB_XFER_ISOCHRONOUS){
        /* Only an admitted audio capture alternate setting, as TinyUSB's audio
         * class opens it for the selected configuration. */
        const struct a733_usb2_audio *a=&host->audio;unsigned packet=tu_edpt_packet_size(e);
        if(!host->audio_mounted||address!=a->address||e->bEndpointAddress!=a->endpoint||!a->starting)return false;
        for(unsigned i=0;i<a->alternates;++i)
            if(a->alternate[i].interface==a->interface&&a->alternate[i].packet==packet&&a->alternate[i].interval==e->bInterval)
                return a733_xhci_open_isoch(&host->core,address,e->bEndpointAddress,packet,e->bInterval,d);
        return false;
    }
    if(e->bmAttributes.xfer==TUSB_XFER_INTERRUPT){
        const struct a733_usb2_hub *hub=hub_at(address);
        if(hub)
            return e->bEndpointAddress==hub->endpoint&&tu_unaligned_read16(&e->wMaxPacketSize)==hub->packet&&
              e->bInterval==hub->interval&&a733_xhci_open_interrupt(&host->core,address,e->bEndpointAddress,hub->packet,e->bInterval,d);
        return (speed!=4||ssdata->ss_ready)&&host->hid_seen&&address==host->hid_address&&e->bEndpointAddress==host->hid.endpoint&&
          tu_unaligned_read16(&e->wMaxPacketSize)==host->hid.packet&&e->bInterval==host->hid.interval&&
          a733_xhci_open_interrupt(&host->core,address,e->bEndpointAddress,host->hid.packet,e->bInterval,d);
    }
    int ok=a733_xhci_open(&host->core,address,e->bEndpointAddress,e->bmAttributes.xfer,tu_edpt_packet_size(e),d);
    if(ok&&speed==4&&(e->bEndpointAddress&15))
        ok=ssdata->ss_ready&&host->msc_seen&&address==host->address&&
           a733_xhci_set_burst(&host->core,address,e->bEndpointAddress,ssdata->ss_burst[!!(e->bEndpointAddress&128)]);
    return ok;
}

bool hcd_edpt_close(uint8_t port,uint8_t address,uint8_t ep)
{(void)port;(void)address;(void)ep;lost();return false;}
static unsigned descriptor_index(unsigned address)
{
    if(!address)return host->core.default_device;
    for(unsigned i=0;i<A733_XHCI_DEVICES;++i)if(host->core.devices[i].address==address)return i;
    return A733_XHCI_DEVICES;
}
bool hcd_setup_send(uint8_t port,uint8_t address,const uint8_t setup[8])
{
    uint64_t d=deadline();if(port||!d||!setup||host->control_buffer)return false;
    unsigned di=descriptor_index(address);if(di>=A733_XHCI_DEVICES)return false;
    struct a733_usb2_descriptor *desc=&host->descriptors[di];
    struct a733_usb2_hub *hub=hub_record(di);const struct a733_usb2_hub *parent=parent_hub(di);
    struct a733_usb2_ss *ssdata=&desc->ss;
    unsigned speed=host->core.devices[di].speed;
    unsigned iso=parent?80+parent->delay_ns:40;
    unsigned len=le16(setup+6),value=le16(setup+2),index=le16(setup+4);int valid=0;
    if(setup[0]==0x80&&setup[1]==6){
        valid=(setup[3]==1&&!setup[2]&&!index&&(len==8||len==18))||
            (setup[3]==2&&!setup[2]&&!index&&(len==9||(desc->configuration_probe&&len==le16(desc->configuration_prefix+2))))||
            (speed==4&&setup[3]==15&&!setup[2]&&!index&&(len==5||(ssdata->ss_bos_bytes&&len==ssdata->ss_bos_bytes)))||
            (setup[3]==3&&len>=2&&len<=254&&!(len&1));
    }else if(speed==4&&!setup[0]&&!index&&((setup[1]==49&&value==iso&&!len&&ssdata->ss_control==2)||(setup[1]==48&&!value&&len==6&&ssdata->ss_control==3)))valid=1;
    else if(!setup[0]&&!index&&!len)valid=(setup[1]==5&&value&&value<=127)||(setup[1]==9&&value==1&&(speed!=4||ssdata->ss_ready));
    else if(desc->kind==A733_USB2_DEVICE_HUB&&hub&&address==hub->address){
        valid=(setup[0]==1&&setup[1]==11&&!index&&value==1&&!len&&hub->protocol==2&&!hub->alt_selected)||
          (setup[0]==0xa0&&setup[1]==6&&value==(speed==4?0x2a00:0x2900)&&!index&&len==(speed==4?12u:9u)&&!hub->ports)||
          (speed==4&&setup[0]==0x20&&setup[1]==12&&!index&&!value&&!len&&hub->ports&&!hub->depth_set)||
          (setup[0]==0x80&&setup[1]==0&&!value&&!index&&len==2&&hub->ports)||
          (setup[0]==0xa0&&setup[1]==0&&!value&&!index&&len==4&&hub->device_power)||
          (setup[0]==0xa3&&setup[1]==0&&!value&&index&&index<=hub->ports&&len==4&&hub->local_power)||
          (setup[0]==0x23&&index&&index<=hub->ports&&!len&&hub->local_power&&
            ((setup[1]==3&&(value==4||value==8||(speed==4&&hub->depth_set&&(value==23||value==24))))||(setup[1]==1&&value>=16&&value<=20)))||
          (setup[0]==0x20&&setup[1]==1&&!index&&!len&&value==0&&hub->local_power);
    }else if(desc->kind==A733_USB2_DEVICE_HID&&!index){
        valid=(setup[0]==0x81&&setup[1]==6&&value==0x2200&&len==host->hid.descriptor_bytes)||
          (setup[0]==0x21&&!len&&!value&&(setup[1]==0x0a||(setup[1]==0x0b&&host->hid.subclass==1)));
    }else if(desc->kind==A733_USB2_DEVICE_AUDIO&&address==host->audio.address){
        /* SET_INTERFACE to a streaming alternate, Feature Unit volume or mute
         * GET requests (UAC1 section 5.2.2.4) and the endpoint sampling
         * frequency SET_CUR (section 5.2.3.2.3.1). */
        const struct a733_usb2_audio *a=&host->audio;unsigned alternate=0;
        for(unsigned i=0;i<a->alternates;++i)alternate|=a->alternate[i].interface==index&&a->alternate[i].alternate==value;
        valid=(setup[0]==1&&setup[1]==11&&!len&&index<32&&(a->streaming&(1u<<index))&&(!value||alternate))||
          (setup[0]==0xa1&&setup[1]>=0x81&&setup[1]<=0x84&&(index&255)==a->control_interface&&
            ((a->features[index>>13]>>((index>>8)&31))&1)&&
            ((value>>8)==1||(value>>8)==2)&&(len==1||len==2))||
          (setup[0]==0x22&&setup[1]==1&&value==0x0100&&index==a->endpoint&&len==3&&a->starting);
    }else valid=desc->kind==A733_USB2_DEVICE_MSC&&setup[0]==0xa1&&setup[1]==0xfe&&!value&&!index&&len==1;
    if(!valid){lost();return false;}
    if(!a733_xhci_setup(&host->core,address,setup,d))return false;
    host->control_address=address;host->control_device=di;memcpy(host->control_setup,setup,8);return true;
}
bool hcd_edpt_xfer(uint8_t port,uint8_t address,uint8_t ep,uint8_t *buffer,uint16_t bytes)
{
    uint64_t d=deadline();if(port||!d)return false;
    if(ep&15){
        struct a733_usb2_hub *hub=hub_at(address);
        if(hub){
            if(ep!=hub->endpoint||bytes!=1||!buffer||hub->status_requested||!hub->local_power||
               hub->powered_mask!=((2u<<hub->ports)-2)||
               host->core.ops.outside(host->core.context,buffer,1)!=1){lost();return false;}
            hub->status_requested=hub->status_deferred=1;hub->status_buffer=buffer;hub->status_bytes=bytes;
            return true;
        }
        if(host->hid_seen&&address==host->hid_address){
            if(!host->hid.pending||host->hid.pending_buffer||ep!=host->hid.endpoint||bytes!=host->hid.packet||!buffer){lost();return false;}
            host->hid.pending_buffer=buffer;host->hid.requested=bytes;
        }else if(host->audio_seen&&address==host->audio.address){
            /* One class request per packet; the poll copies the oldest
             * completed packet into it. */
            struct a733_usb2_audio *a=&host->audio;
            if(!host->audio_mounted||(!a->starting&&!a->started)||a->pending_buffer||ep!=a->endpoint||bytes!=a->packet||
               !buffer||host->core.ops.outside(host->core.context,buffer,bytes)!=1||
               !a733_xhci_isoch_start(&host->core,address,ep)){lost();return false;}
            a->pending_buffer=buffer;a->pending_bytes=bytes;return true;
        }else if(!host->msc_seen||address!=host->address||!bot_submit(ep,buffer,bytes)){lost();return false;}
    }
    if(host->audio_seen&&address==host->audio.address&&ep==0&&bytes&&
       (host->control_setup[0]!=0x22||bytes!=3||!buffer||le24(buffer)!=host->audio.rate)){lost();return false;}
    if(a733_xhci_device_speed(&host->core,address)==4&&ep==0&&bytes&&
       (host->control_setup[1]!=48||bytes!=6||!buffer||memcmp(buffer,host->descriptors[host->control_device].ss.ss_sel,6))){lost();return false;}
    int ok=a733_xhci_transfer(&host->core,address,ep,buffer,bytes,d);
    if(!ok&&(ep&15))lost();
    if(ok&&ep==128&&bytes){host->control_buffer=buffer;host->control_bytes=bytes;}
    return ok;
}
bool hcd_edpt_abort_xfer(uint8_t port,uint8_t address,uint8_t ep)
{return owner()&&!port&&a733_xhci_abort(&host->core,address,ep);}
/* Bulk clear-halt/BOT reset recovery is outside the fixed GET_MAX_LUN path. */
bool hcd_edpt_clear_stall(uint8_t port,uint8_t address,uint8_t ep)
{(void)port;(void)address;(void)ep;lost();return false;}
bool hcd_dcache_clean(const void *p,uint32_t z){(void)p;(void)z;return owner();}
bool hcd_dcache_invalidate(const void *p,uint32_t z){(void)p;(void)z;return owner();}
bool hcd_dcache_clean_invalidate(const void *p,uint32_t z){(void)p;(void)z;return owner();}
void tuh_msc_mount_cb(uint8_t address)
{
    if(!owner()||!host->msc_seen||address!=host->address||host->msc_mounted||host->bot_phase||host->bot_stage){lost();return;}
    host->blocks=tuh_msc_get_block_count(address,0);host->block_bytes=tuh_msc_get_block_size(address,0);
    if(!host->blocks||host->block_bytes!=512||host->bot_status){lost();return;}
    host->address=address;host->msc_mounted=1;host->read_failed=host->read_pending=host->read_complete=0;mounted_update();
}
/* TinyUSB closes a class only while removing its device; the device close
 * that follows decides whether its slot can be torn down. */
void tuh_msc_umount_cb(uint8_t address)
{
    if(!owner()||!host->msc_seen||address!=host->address){lost();return;}
    withdraw_msc();mounted_update();
}
static bool read_complete(uint8_t address,const tuh_msc_complete_data_t *d)
{
    if(!owner()||address!=host->address||!host->read_pending||!d||d->user_arg!=(uintptr_t)host||
        d->scsi_data!=host->sector||host->bot_phase||host->bot_stage||host->bot_opcode!=0x28||
        host->bot_received!=512||host->bot_status||d->csw->status){lost();return false;}
    host->read_complete=1;return true;
}
int a733_usb2_read_sector(struct a733_usb2_native *n,uint32_t lba,uint64_t d)
{
    uint64_t now;if(n!=host||!clock_now(&now)||!n->msc_mounted||n->read_pending||n->read_complete||
        n->read_failed||lba>=n->blocks||d<=now||d>n->platform.config.lifetime_deadline)return 0;
    if(n->core.ops.live(n->core.context,n->core.generation,d)!=1)return 0;
    n->read_deadline=d;n->read_pending=1;
    if(!tuh_msc_read10((uint8_t)n->address,0,n->sector,lba,1,read_complete,(uintptr_t)n)){lost();return 0;}
    return 1;
}
int a733_usb2_read_result(struct a733_usb2_native *n,uint8_t out[512])
{
    uint64_t now;if(n!=host||!clock_now(&now)||n->read_failed)return -1;
    if(!n->read_pending||!n->read_complete)return 0;
    if(now>=n->read_deadline){lost();return -1;}
    if(!out||!a733_usb2_platform_outside(&n->platform,out,512))return -1;
    int live=n->core.ops.live(n->core.context,n->core.generation,n->read_deadline);
    if(live<0){lost();return -1;}if(!live)return 0;
    memcpy(out,n->sector,512);n->read_pending=0;n->read_complete=0;return 1;
}
void tuh_hid_mount_cb(uint8_t address,uint8_t instance,const uint8_t *descriptor,uint16_t bytes)
{
    uint64_t now;
    if(!owner()||!host->hid_seen||address!=host->hid_address||host->hid_mounted||instance||
       !a733_xhci_device_speed(&host->core,address)||!descriptor||bytes!=host->hid.descriptor_bytes||
       !bytes||bytes>sizeof(host->hid.descriptor)||!clock_now(&now)||
       host->core.ops.live(host->core.context,host->core.generation,mount_bound())!=1){lost();return;}
    memcpy(host->hid.descriptor,descriptor,bytes);if(!host->dual_leaf)host->address=address;host->hid_mounted=1;mounted_update();
}
void tuh_hid_umount_cb(uint8_t address,uint8_t instance)
{
    if(!owner()||!host->hid_seen||address!=host->hid_address||instance){lost();return;}
    withdraw_hid();mounted_update();
}
void tuh_hid_report_received_cb(uint8_t address,uint8_t instance,const uint8_t *report,uint16_t bytes)
{
    if(!owner()){lost();return;}
    uint64_t now;struct a733_usb2_hid *h=&host->hid;
    /* Pinned TinyUSB ignores result in hidh_xfer_cb. Only the HCD's validated
     * successful event may reach this callback, including actual short/ZLP. */
    if(!owner()||!host->hid_mounted||address!=host->hid_address||instance||
       !h->pending||h->complete||!h->event_seen||report!=h->pending_buffer||
       bytes!=h->event_bytes||bytes>sizeof(h->copied)||h->sequence==UINT64_MAX||
       !clock_now(&now)||now>=h->deadline||
       host->core.ops.live(host->core.context,host->core.generation,h->deadline)!=1){lost();return;}
    memset(h->copied,0,sizeof(h->copied));memcpy(h->copied,report,bytes);
    h->pending_buffer=NULL;++h->sequence;h->complete=1;
}
enum a733_usb2_device_kind a733_usb2_device_kind(struct a733_usb2_native *n)
{return n==host&&owner()&&n->mounted?n->kind:A733_USB2_DEVICE_NONE;}
int a733_usb2_hid_receive(struct a733_usb2_native *n,uint64_t d)
{
    uint64_t now;
    if(n!=host||!clock_now(&now)||!n->hid_mounted||
       n->hid.pending||n->hid.complete||d<=now||d>n->platform.config.lifetime_deadline)return 0;
    if(n->core.ops.live(n->core.context,n->core.generation,d)!=1)return 0;
    n->hid.deadline=d;n->hid.pending=1;n->hid.event_seen=0;n->hid.event_bytes=0;
    if(!tuh_hid_receive_report((uint8_t)n->hid_address,0)){lost();return 0;}return 1;
}
int a733_usb2_hid_result(struct a733_usb2_native *n,struct a733_usb2_hid_report *out)
{
    uint64_t now;
    if(n!=host||!clock_now(&now)||!n->hid_mounted)return -1;
    if(!n->hid.pending||!n->hid.complete)return 0;
    if(now>=n->hid.deadline){lost();return -1;}
    if(!out||!a733_usb2_platform_outside(&n->platform,out,sizeof(*out)))return -1;
    int live=n->core.ops.live(n->core.context,n->core.generation,n->hid.deadline);
    if(live<0){lost();return -1;}if(!live)return 0;
    struct a733_usb2_hid_report value={.generation=n->core.generation,.sequence=n->hid.sequence,
      .bytes=n->hid.event_bytes,.address=(uint8_t)n->hid_address,.interface=0,.protocol=(uint8_t)n->hid.protocol,
      .endpoint=(uint8_t)n->hid.endpoint,.speed=(uint8_t)a733_xhci_device_speed(&n->core,n->hid_address)};
    memcpy(value.data,n->hid.copied,value.bytes);
    if(!clock_now(&now)||now>=n->hid.deadline||
       n->core.ops.live(n->core.context,n->core.generation,n->hid.deadline)!=1){lost();return -1;}
    memcpy(out,&value,sizeof(value));n->hid.pending=n->hid.complete=n->hid.event_seen=0;return 1;
}
#if CFG_TUH_AUDIO
/* The single audio function is TinyUSB audio index 0. */
static unsigned capture_stream(void)
{
    for(unsigned i=0;i<tuh_audio_stream_count(0);++i)
        if(tuh_audio_stream_direction(0,(uint8_t)i)==TUH_AUDIO_STREAM_CAPTURE)return i;
    return 0xff;
}
void tuh_audio_mount_cb(uint8_t index)
{
    struct a733_usb2_audio *a=&host->audio;
    if(!owner()||index||!host->audio_seen||host->audio_mounted||tuh_audio_get_dev_addr(0)!=a->address||
       capture_stream()==0xff||host->core.ops.live(host->core.context,host->core.generation,mount_bound())!=1){lost();return;}
    a->stream=capture_stream();host->audio_mounted=1;mounted_update();
}
void tuh_audio_umount_cb(uint8_t index)
{
    if(!owner()||index||!host->audio_seen){lost();return;}
    withdraw_audio();mounted_update();
}
void tuh_audio_capture_cb(uint8_t index,uint8_t stream,uint16_t bytes)
{
    struct a733_usb2_audio *a=&host->audio;
    if(!owner()||index||stream!=a->stream||!a->started||!a->channels){lost();return;}
    a->written+=bytes/(2u*a->channels);
}
/* Start completes after SET_INTERFACE, the optional sampling-frequency
 * SET_CUR and the first packet request. Any failure ends the session. */
void tuh_audio_event_cb(uint8_t index,uint8_t stream,tuh_audio_event_t event,tusb_xfer_result_t result)
{
    struct a733_usb2_audio *a=&host->audio;
    if(!owner()||index||stream!=a->stream||event!=TUH_AUDIO_EVENT_START_COMPLETE||
       result!=XFER_RESULT_SUCCESS||!a->starting){lost();return;}
    a->starting=0;a->started=1;
}
int a733_usb2_audio_start(struct a733_usb2_native *n,uint32_t rate,unsigned channels,uint64_t d)
{
    uint64_t now;struct a733_usb2_audio *a=&n->audio;
    if(n!=host||!clock_now(&now)||!n->audio_mounted||a->starting||a->started||!rate||!channels||
       d<=now||d>n->platform.config.lifetime_deadline||n->core.ops.live(n->core.context,n->core.generation,d)!=1)return 0;
    unsigned config=0xff;
    for(unsigned i=0;i<tuh_audio_config_count(0,(uint8_t)a->stream);++i){
        tuh_audio_stream_config_t c;
        if(tuh_audio_config_get(0,(uint8_t)a->stream,(uint8_t)i,&c)&&c.dir==TUH_AUDIO_STREAM_CAPTURE&&
           c.format==TUH_AUDIO_FORMAT_S16_LE&&c.sample_rate==rate&&c.channels==channels){config=i;break;}
    }
    if(config==0xff)return 0;
    a->rate=rate;a->channels=channels;a->start_deadline=d;a->starting=1;a->written=a->read=0;
    if(!tuh_audio_configure(0,(uint8_t)a->stream,(uint8_t)config)){lost();return 0;}
    a->packet=0;
    for(unsigned i=0;i<A733_XHCI_DEVICES;++i){
        const struct a733_xhci_device *dev=&n->core.devices[i];
        if(dev->address==a->address&&dev->endpoints[1].transfer_type==1)a->packet=dev->endpoints[1].packet;
    }
    if(!a->packet||!tuh_audio_start(0,(uint8_t)a->stream)){lost();return 0;}
    return 1;
}
int a733_usb2_audio_read(struct a733_usb2_native *n,struct a733_usb2_audio_block *out)
{
    uint64_t now;struct a733_usb2_audio *a=&n->audio;
    if(n!=host||!clock_now(&now)||!n->audio_mounted||!a->started)return -1;
    if(!out||!a733_usb2_platform_outside(&n->platform,out,sizeof(*out)))return -1;
    uint32_t available=tuh_audio_read_available(0,(uint8_t)a->stream),limit=A733_USB2_AUDIO_BLOCK_BYTES/(2u*a->channels);
    if(!available)return 0;
    int live=n->core.ops.live(n->core.context,n->core.generation,n->platform.config.lifetime_deadline);
    if(live<0){lost();return -1;}if(!live)return 0;
    memset(out,0,sizeof(*out));
    uint32_t frames=tuh_audio_read(0,(uint8_t)a->stream,out->data,available<limit?available:limit);
    if(!frames||frames>available){lost();return -1;}
    a->read+=frames;
    uint64_t held=a->read+tuh_audio_read_available(0,(uint8_t)a->stream),overwritten=a->written>held?a->written-held:0;
    out->generation=n->core.generation;out->sequence=++a->sequence;out->rate=a->rate;out->frames=frames;
    out->dropped=n->core.isoch.dropped;out->overruns=n->core.isoch.overruns;
    out->overwritten=overwritten>UINT32_MAX?UINT32_MAX:(uint32_t)overwritten;
    out->address=(uint8_t)a->address;out->interface=(uint8_t)a->interface;out->endpoint=(uint8_t)a->endpoint;
    out->speed=(uint8_t)a733_xhci_device_speed(&n->core,a->address);out->channels=(uint8_t)a->channels;out->sample_bytes=2;
    return 1;
}
#else
int a733_usb2_audio_start(struct a733_usb2_native *n,uint32_t rate,unsigned channels,uint64_t d)
{(void)n;(void)rate;(void)channels;(void)d;return 0;}
int a733_usb2_audio_read(struct a733_usb2_native *n,struct a733_usb2_audio_block *out){(void)n;(void)out;return -1;}
#endif
