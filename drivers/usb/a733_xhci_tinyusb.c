/* SPDX-License-Identifier: GPL-2.0-only */
#include "a733_usb2_native.h"
#include "tusb.h"
#include "host/hcd.h"
#include <string.h>
static struct a733_usb2_native *host;
static void mounted_update(void)
{
    host->mounted=host->dual_leaf?(host->msc_mounted&&host->hid_mounted):(host->msc_mounted||host->hid_mounted);
}
static uint32_t le32(const uint8_t *p)
{return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static int owner(void)
{return host&&host->self==host&&host->core_started&&!host->failed&&host->core.ops.current(host->core.context)==1;}
static int clock_now(uint64_t *v)
{
    if(!owner()||host->core.ops.now(host->core.context,v)!=1||*v<host->last_now||
        *v>=host->platform.config.lifetime_deadline)return 0;
    host->last_now=*v;return 1;
}
static uint64_t deadline(void)
{
    uint64_t now;if(!clock_now(&now)||now>UINT64_MAX-1000000)return 0;
    uint64_t d=host->hid.pending?host->hid.deadline:now+1000000;
    if(!host->mounted&&d>host->platform.config.deadline)d=host->platform.config.deadline;
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
uint32_t tusb_time_millis_api(void)
{uint64_t now;if(!clock_now(&now)){lost();return host?(uint32_t)(host->last_now/1000):0;}return (uint32_t)(now/1000);}
int a733_usb2_host_bind(struct a733_usb2_native *n)
{
    if(host||!n||n->self!=n||!n->core_started||!n->core.self||n->core.ops.current(n->core.context)!=1)return 0;
    host=n;const tusb_rhport_init_t init={.role=TUSB_ROLE_HOST,.speed=n->platform.config.superspeed?TUSB_SPEED_SUPER:TUSB_SPEED_HIGH};
    if(!tusb_init(0,&init)){lost();return 0;}return 1;
}
int a733_usb2_host_poll(struct a733_usb2_native *n)
{
    uint64_t now;if(n!=host||!clock_now(&now)){lost();return -1;}
    if((!host->mounted&&now>=host->platform.config.deadline)||
        (host->read_pending&&now>=host->read_deadline)||
        (host->hid.pending&&now>=host->hid.deadline)){lost();return -1;}
    if(host->hub.status_deferred&&now>=host->hub.power_good_deadline&&host->core.devices[0].hub_configured){
        if(!a733_xhci_transfer(&host->core,host->hub.address,host->hub.endpoint,
           host->hub.status_buffer,host->hub.status_bytes,host->platform.config.lifetime_deadline)){
            lost();return -1;
        }
        host->hub.status_deferred=0;
    }
    enum a733_xhci_result r=a733_xhci_poll(&host->core);
    if(r==A733_XHCI_RETAINED||r==A733_XHCI_INVALID){lost();return -1;}
    /* Notice validation records metadata only. Apply it after core callbacks
     * return, before the queued TinyUSB completion can issue another request. */
    if(!host->failed&&host->hub.mark_pending){
        if(!a733_xhci_mark_hub(&host->core,host->hub.mark_address))lost();
        host->hub.mark_pending=0;
    }
    if(!host->failed&&host->hub.configure_pending){
        if(!a733_xhci_hub_configure(&host->core,host->hub.address,host->hub.ports,host->hub.ttt,host->hub.alt_selected))lost();
        host->hub.configure_pending=0;
    }
    if(!host->failed&&r!=A733_XHCI_BUSY)tuh_task_ext(0,false);
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
static int ss_bos(const uint8_t *p,unsigned bytes)
{
    struct a733_usb2_native *n=host;
    if(bytes<5||p[0]!=5||p[1]!=15||le16(p+2)<15||le16(p+2)>sizeof(n->ss_bos)||!p[4])return 0;
    if(bytes==5){if(n->ss_control)return 0;memcpy(n->ss_bos_prefix,p,5);n->ss_bos_bytes=le16(p+2);n->ss_control=1;return 1;}
    if(n->ss_control!=1||bytes!=n->ss_bos_bytes||memcmp(n->ss_bos_prefix,p,5))return 0;
    unsigned count=0,seen=0;
    for(unsigned at=5;at<bytes;){
        if(bytes-at<3||p[at]<3||p[at]>bytes-at||p[at+1]!=16)return 0;
        const uint8_t *c=p+at;
        if(c[2]==3){
            if(seen++||c[0]!=10||(c[3]&~2u)||!(le16(c+4)&8)||le16(c+4)&~15u||
               c[6]>3||!(le16(c+4)&(1u<<c[6]))||c[7]>10||le16(c+8)>2047)return 0;
            n->ss_u1=c[7];n->ss_u2=le16(c+8);
        }else if(c[2]==2){if(c[0]!=7||(le32(c+3)&1))return 0;}
        else if(c[2]==4&&c[0]!=20)return 0;
        ++count;at+=c[0];
    }
    if(seen!=1||count!=p[4])return 0;
    memcpy(n->ss_bos,p,bytes);n->ss_control=2;return 1;
}
bool a733_tinyusb_ss_enumerate(uint8_t address,unsigned stage,uint8_t *buffer,tuh_xfer_cb_t callback,uintptr_t next)
{
    if(!owner()||!host->platform.config.superspeed||!address||!buffer||stage!=host->ss_control||stage>3||
       host->core.ops.outside(host->core.context,buffer,256)!=1){lost();return false;}
    tusb_control_request_t request={0};request.bRequest=stage<2?6:stage==2?49:48;
    request.bmRequestType=stage<2?0x80:0;request.wValue=stage<2?0x0f00:stage==2?40:0;
    request.wLength=stage==0?5:stage==1?(uint16_t)host->ss_bos_bytes:stage==3?6:0;
    if(stage==3){
        unsigned u1=host->ss_u1>host->core.root_u1?host->ss_u1:host->core.root_u1;
        unsigned u2=host->ss_u2>host->core.root_u2?host->ss_u2:host->core.root_u2;
        if(!u1)u1=1;if(!u2)u2=1;
        host->ss_sel[0]=host->ss_sel[1]=(uint8_t)u1;
        host->ss_sel[2]=host->ss_sel[4]=(uint8_t)u2;host->ss_sel[3]=host->ss_sel[5]=(uint8_t)(u2>>8);
        memcpy(buffer,host->ss_sel,6);
    }
    host->ss_request=stage;
    tuh_xfer_t xfer={.daddr=address,.ep_addr=0,.setup=&request,.buffer=request.wLength?buffer:NULL,.complete_cb=callback,.user_data=next};
    return tuh_control_xfer(&xfer);
}
static int control_response(const struct a733_xhci_notice *e)
{
    struct a733_usb2_native *n=host;
    if(n->control_device>=A733_XHCI_DEVICES||e->address!=n->control_address)return 0;
    struct a733_usb2_descriptor *desc=&n->descriptors[n->control_device];
    struct a733_xhci_device *device=&n->core.devices[n->control_device];
    struct a733_usb2_hub *hub=&n->hub;
    const uint8_t *p=n->control_buffer,*s=n->control_setup;unsigned bytes=n->control_bytes;
    if(!p){
        if(n->platform.config.superspeed&&device->control_stage==0&&e->bytes==0&&e->endpoint==128){
            if(s[1]==49){if(n->ss_control!=2||n->ss_request!=2)return 0;n->ss_control=3;}
            if(s[1]==48){if(n->ss_control!=3||n->ss_request!=3)return 0;n->ss_control=4;n->ss_ready=1;}
        }
        if(device->control_stage==0&&e->bytes==0&&e->endpoint==128&&desc->kind==A733_USB2_DEVICE_HUB){
            uint64_t now;
            if(s[0]==1&&s[1]==11&&le16(s+2)==1)hub->alt_selected=1;
            if(s[0]==0x23&&s[1]==3&&le16(s+2)==8){
                unsigned port=le16(s+4);if(!port||port>hub->ports||!clock_now(&now))return 0;
                hub->powered_mask|=1u<<port;
                if(hub->powered_mask==((2u<<hub->ports)-2)){
                    uint64_t delay=(uint64_t)hub->power_good_2ms*2000;
                    if(now>UINT64_MAX-delay)return 0;hub->power_good_deadline=now+delay;
                }
            }
        }
        return 1;
    }
    if(e->endpoint!=128||e->result||e->bytes!=bytes||bytes!=le16(s+6))return 0;
    n->control_buffer=NULL;n->control_bytes=0;
    if(n->platform.config.superspeed&&s[0]==0x80&&s[1]==6&&s[3]==15)return ss_bos(p,bytes);
    if(desc->kind==A733_USB2_DEVICE_HUB){
        if(s[0]==0xa0&&s[1]==6&&le16(s+2)==0x2900){
            unsigned chars=bytes>=5?le16(p+3):0;
            if(bytes!=9||p[0]!=9||p[1]!=0x29||!p[2]||p[2]>7||chars&~255u||
               (chars&3)>1||((chars>>3)&3)>1||p[7]&1||p[7]&~((2u<<p[2])-2)||p[8]!=255)return 0;
            if(hub->protocol==2&&!hub->alt_selected)return 0;
            hub->ports=p[2];hub->ttt=(chars>>5)&3;hub->power_good_2ms=p[5];
            hub->configure_pending=1;return 1;
        }
        if(s[1]==0&&s[0]==0x80){
            if(bytes!=2||(le16(p)&~3u)||!(p[0]&1))return 0;hub->device_power=1;return 1;
        }
        if(s[1]==0&&s[0]==0xa0){
            if(bytes!=4||!hub->device_power||p[0]||p[1]||p[3]||(p[2]&~1u))return 0;
            hub->local_power=1;return 1;
        }
        if(s[1]==0&&s[0]==0xa3){
            unsigned port=le16(s+4),status=bytes==4?le16(p):0,changes=bytes==4?le16(p+2):0;
            if(bytes!=4||!port||port>hub->ports||status&~0x1f1fu||changes&~31u||
               status&(8u|4u|2048u)||changes&8u||!(status&256u)||
               (status&1536u)==1536u)return 0;
            if(status&1){
                if(!(hub->connected_mask&(1u<<port))){
                    unsigned mask=hub->connected_mask;
                    if(mask&&(!n->dual_leaf||(mask&(mask-1))))return 0;
                    hub->connected_mask|=1u<<port;hub->selected_port=port;
                }
            }else if(hub->connected_mask&(1u<<port))return 0;
            for(unsigned j=1;j<A733_XHCI_DEVICES;++j)
                if(n->core.devices[j].parent_port==port&&n->core.devices[j].zero_retired&&
                   (!(status&2)||(changes&2)))return 0;
            return 1;
        }
    }
    if(s[0]==0xa1&&s[1]==0xfe)return desc->kind==A733_USB2_DEVICE_MSC&&bytes==1&&p[0]==0;
    if(s[0]==0x81&&s[1]==6&&s[3]==0x22)return desc->kind==A733_USB2_DEVICE_HID&&bytes==n->hid.descriptor_bytes;
    if(s[0]!=0x80||s[1]!=6||bytes<2||p[1]!=s[3])return 0;
    if(s[3]==1){
        unsigned speed=device->speed,is_hub=p[4]==9;
        if(speed==4&&(is_hub||p[2]||p[3]!=3||p[7]!=9))return 0;
        if(p[0]!=18||p[5]||(!is_hub&&(p[4]||p[6]))||
           (is_hub&&(n->control_device||speed!=3||(p[6]!=1&&p[6]!=2)))||
           (speed==2&&p[7]!=8)||(speed==3&&p[7]!=64)||
           (speed==1&&p[7]!=8&&p[7]!=16&&p[7]!=32&&p[7]!=64))return 0;
        if(bytes==8){
            if(desc->device_probe)return 0;memcpy(desc->device_prefix,p,8);desc->device_probe=1;
            if(is_hub){
                hub->mark_pending=1;hub->mark_address=e->address;
                desc->kind=A733_USB2_DEVICE_HUB;hub->present=1;hub->protocol=p[6];
            }
            return 1;
        }
        if(bytes!=18||!desc->device_probe||memcmp(desc->device_prefix,p,8)||p[17]!=1)return 0;
        if(is_hub)hub->address=e->address;return 1;
    }
    if(s[3]==2){
        unsigned total=bytes>=4?le16(p+2):0,is_hub=desc->kind==A733_USB2_DEVICE_HUB,ss=device->speed==4;
        if(ss&&(!n->ss_ready||(p[8]*8u>900u||p[8]*8u>n->platform.typec->config.source_ma)))return 0;
        if(bytes<9||p[0]!=9||p[4]!=1||p[5]!=1||!(p[7]&128)||p[8]>250||
           (is_hub?(total!=(hub->protocol==2?41u:25u)||(p[7]&0xc0)!=0xc0||(p[7]&31)):(ss?total!=44:(total!=32&&total!=34))))return 0;
        if(bytes==9){if(desc->configuration_probe)return 0;memcpy(desc->configuration_prefix,p,9);desc->configuration_probe=1;return 1;}
        if(bytes!=total||!desc->configuration_probe||memcmp(desc->configuration_prefix,p,9)||p[9]!=9||p[10]!=4||p[11]||p[12])return 0;
        if(is_hub){
            for(unsigned alt=0;alt<(hub->protocol==2?2u:1u);++alt){
                unsigned at=9+16*alt;const uint8_t *q=p+at;
                if(q[0]!=9||q[1]!=4||q[2]||q[3]!=alt||q[4]!=1||q[5]!=9||q[6]||q[7]!=alt+1||
                   q[9]!=7||q[10]!=5||q[11]&~0x8fu||!(q[11]&128)||!(q[11]&15)||q[12]!=3||
                   !le16(q+13)||le16(q+13)>64||!q[15]||q[15]>16)return 0;
                if(!alt){hub->endpoint=q[11];hub->packet=le16(q+13);hub->interval=q[15];}
                else if(q[11]!=hub->endpoint||le16(q+13)!=hub->packet||q[15]!=hub->interval)return 0;
            }
            return 1;
        }
        if(bytes==34){
            unsigned ep=p[29],packet=le16(p+31),interval=p[33];
            if(p[13]!=1||p[14]!=3||p[15]>1||p[16]>2||(!p[15]&&p[16])||
               (p[15]&&!p[16])||p[18]!=9||p[19]!=0x21||p[23]!=1||p[24]!=0x22||
               !le16(p+25)||le16(p+25)>256||p[27]!=7||p[28]!=5||
               ep&~0x8fu||!(ep&128)||!(ep&15)||p[30]!=3||!packet||
               packet>(device->speed==2?8u:64u)||!interval||interval>(device->speed==3?16u:255u))return 0;
            if(n->hid_seen||(n->msc_seen&&!n->dual_leaf))return 0;
            n->hid_seen=1;n->hid_address=e->address;
            desc->kind=A733_USB2_DEVICE_HID;n->kind=n->msc_seen?A733_USB2_DEVICE_DUAL:A733_USB2_DEVICE_HID;n->hid.endpoint=ep;n->hid.packet=packet;n->hid.interval=interval;
            n->hid.subclass=p[15];n->hid.protocol=p[16];n->hid.descriptor_bytes=le16(p+25);return 1;
        }
        if((device->speed!=3&&device->speed!=4)||p[13]!=2||p[14]!=8||p[15]!=6||p[16]!=0x50)return 0;
        unsigned dirs=0,seen=0;
        for(unsigned at=18;at<total;at+=ss?13:7){unsigned ep=p[at+2];
            if(p[at]!=7||p[at+1]!=5||(ep&0x70)||!(ep&15)||p[at+3]!=2||le16(p+at+4)!=(ss?1024u:512u))return 0;
            if(ss){const uint8_t *c=p+at+7;if(c[0]!=6||c[1]!=0x30||c[2]>15||c[3]>16||le16(c+4))return 0;n->ss_burst[!!(ep&128)]=c[2];}
            if(ep&128)n->msc_in=ep;else n->msc_out=ep;
            unsigned bit=1u<<((ep&15)+((ep&128)?16:0));if(seen&bit)return 0;seen|=bit;dirs|=(ep&128)?2:1;}
        if(dirs!=3||n->msc_seen||(n->hid_seen&&!n->dual_leaf))return 0;
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
    if(event->kind==A733_XHCI_ATTACH)hcd_event_device_attach(0,false);
    else if(event->kind==A733_XHCI_REMOVE){host->mounted=0;host->read_failed=1;hcd_event_device_remove(0,false);}
    else if(event->kind==A733_XHCI_TRANSFER){
        uint64_t now,d=deadline();
        if(!d||!clock_now(&now)||host->core.ops.live(host->core.context,host->core.generation,d)!=1){lost();return;}
        if(event->endpoint&15){
            if(n->hub.present&&event->address==n->hub.address){
                struct a733_usb2_hub *h=&n->hub;
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
{return owner()&&!port&&i&&i->role==TUSB_ROLE_HOST&&i->speed==(host->platform.config.superspeed?TUSB_SPEED_SUPER:TUSB_SPEED_HIGH);}
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
void hcd_device_close(uint8_t port,uint8_t address)
{
    if(owner()&&!port&&!address&&a733_xhci_retire_zero(&host->core))return;
    lost();
}

bool hcd_edpt_open(uint8_t port,uint8_t address,const tusb_desc_endpoint_t *e)
{
    uint64_t d=deadline();if(port||!e||!d)return false;
    if(!address&&host->hub.present&&host->core.devices[0].zero_retired){
        tuh_bus_info_t bus;
        if(!tuh_bus_info_get(0,&bus)||bus.rhport||bus.hub_addr!=host->hub.address||
           !bus.hub_port||bus.hub_port>7||!(host->hub.connected_mask&(1u<<bus.hub_port))||
           !a733_xhci_bind_child(&host->core,bus.hub_addr,bus.hub_port,
             bus.speed==TUSB_SPEED_HIGH?3:bus.speed==TUSB_SPEED_LOW?2:bus.speed==TUSB_SPEED_FULL?1:0,d)){lost();return false;}
    }
    if(e->bmAttributes.xfer==TUSB_XFER_INTERRUPT){
        if(host->hub.present&&address==host->hub.address)
            return e->bEndpointAddress==host->hub.endpoint&&tu_unaligned_read16(&e->wMaxPacketSize)==host->hub.packet&&
              e->bInterval==host->hub.interval&&a733_xhci_open_interrupt(&host->core,address,e->bEndpointAddress,host->hub.packet,e->bInterval,d);
        return host->hid_seen&&address==host->hid_address&&e->bEndpointAddress==host->hid.endpoint&&
          tu_unaligned_read16(&e->wMaxPacketSize)==host->hid.packet&&e->bInterval==host->hid.interval&&
          a733_xhci_open_interrupt(&host->core,address,e->bEndpointAddress,host->hid.packet,e->bInterval,d);
    }
    int ok=a733_xhci_open(&host->core,address,e->bEndpointAddress,e->bmAttributes.xfer,tu_edpt_packet_size(e),d);
    if(ok&&host->platform.config.superspeed&&(e->bEndpointAddress&15))
        ok=host->ss_ready&&host->msc_seen&&address==host->address&&
           a733_xhci_set_burst(&host->core,address,e->bEndpointAddress,host->ss_burst[!!(e->bEndpointAddress&128)]);
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
    struct a733_usb2_hub *hub=&host->hub;
    unsigned len=le16(setup+6),value=le16(setup+2),index=le16(setup+4);int valid=0;
    if(setup[0]==0x80&&setup[1]==6){
        valid=(setup[3]==1&&!setup[2]&&!index&&(len==8||len==18))||
            (setup[3]==2&&!setup[2]&&!index&&(len==9||(desc->configuration_probe&&len==le16(desc->configuration_prefix+2))))||
            (host->platform.config.superspeed&&setup[3]==15&&!setup[2]&&!index&&(len==5||(host->ss_bos_bytes&&len==host->ss_bos_bytes)))||
            (setup[3]==3&&len>=2&&len<=254&&!(len&1));
    }else if(host->platform.config.superspeed&&!setup[0]&&!index&&((setup[1]==49&&value==40&&!len&&host->ss_control==2)||(setup[1]==48&&!value&&len==6&&host->ss_control==3)))valid=1;
    else if(!setup[0]&&!index&&!len)valid=(setup[1]==5&&value&&value<=127)||(setup[1]==9&&value==1&&(!host->platform.config.superspeed||host->ss_ready));
    else if(desc->kind==A733_USB2_DEVICE_HUB&&address==hub->address){
        valid=(setup[0]==1&&setup[1]==11&&!index&&value==1&&!len&&hub->protocol==2&&!hub->alt_selected)||
          (setup[0]==0xa0&&setup[1]==6&&value==0x2900&&!index&&len==9&&!hub->ports)||
          (setup[0]==0x80&&setup[1]==0&&!value&&!index&&len==2&&hub->ports)||
          (setup[0]==0xa0&&setup[1]==0&&!value&&!index&&len==4&&hub->device_power)||
          (setup[0]==0xa3&&setup[1]==0&&!value&&index&&index<=hub->ports&&len==4&&hub->local_power)||
          (setup[0]==0x23&&index&&index<=hub->ports&&!len&&hub->local_power&&
            ((setup[1]==3&&(value==4||value==8))||(setup[1]==1&&value>=16&&value<=20)))||
          (setup[0]==0x20&&setup[1]==1&&!index&&!len&&value==0&&hub->local_power);
    }else if(desc->kind==A733_USB2_DEVICE_HID&&!index){
        valid=(setup[0]==0x81&&setup[1]==6&&value==0x2200&&len==host->hid.descriptor_bytes)||
          (setup[0]==0x21&&!len&&!value&&(setup[1]==0x0a||(setup[1]==0x0b&&host->hid.subclass==1)));
    }else valid=desc->kind==A733_USB2_DEVICE_MSC&&setup[0]==0xa1&&setup[1]==0xfe&&!value&&!index&&len==1;
    if(!valid){lost();return false;}
    if(!a733_xhci_setup(&host->core,address,setup,d))return false;
    host->control_address=address;host->control_device=di;memcpy(host->control_setup,setup,8);return true;
}
bool hcd_edpt_xfer(uint8_t port,uint8_t address,uint8_t ep,uint8_t *buffer,uint16_t bytes)
{
    uint64_t d=deadline();if(port||!d)return false;
    if(ep&15){
        if(host->hub.present&&address==host->hub.address){
            struct a733_usb2_hub *hub=&host->hub;
            if(ep!=hub->endpoint||bytes!=1||!buffer||hub->status_requested||!hub->local_power||
               hub->powered_mask!=((2u<<hub->ports)-2)||
               host->core.ops.outside(host->core.context,buffer,1)!=1){lost();return false;}
            hub->status_requested=hub->status_deferred=1;hub->status_buffer=buffer;hub->status_bytes=bytes;
            return true;
        }
        if(host->hid_seen&&address==host->hid_address){
            if(!host->hid.pending||host->hid.pending_buffer||ep!=host->hid.endpoint||bytes!=host->hid.packet||!buffer){lost();return false;}
            host->hid.pending_buffer=buffer;host->hid.requested=bytes;
        }else if(!host->msc_seen||address!=host->address||!bot_submit(ep,buffer,bytes)){lost();return false;}
    }
    if(host->platform.config.superspeed&&ep==0&&bytes&&
       (host->control_setup[1]!=48||bytes!=6||!buffer||memcmp(buffer,host->ss_sel,6))){lost();return false;}
    int ok=a733_xhci_transfer(&host->core,address,ep,buffer,bytes,d);
    if(!ok&&(ep&15))lost();
    if(ok&&ep==128&&bytes){host->control_buffer=buffer;host->control_bytes=bytes;}
    return ok;
}
bool hcd_edpt_abort_xfer(uint8_t port,uint8_t address,uint8_t ep)
{return owner()&&!port&&a733_xhci_abort(&host->core,address,ep);}
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
    host->address=address;host->msc_mounted=1;mounted_update();
}
void tuh_msc_umount_cb(uint8_t address){(void)address;lost();}
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
       host->core.ops.live(host->core.context,host->core.generation,host->platform.config.deadline)!=1){lost();return;}
    memcpy(host->hid.descriptor,descriptor,bytes);if(!host->dual_leaf)host->address=address;host->hid_mounted=1;mounted_update();
}
void tuh_hid_umount_cb(uint8_t address,uint8_t instance)
{(void)address;(void)instance;lost();}
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
