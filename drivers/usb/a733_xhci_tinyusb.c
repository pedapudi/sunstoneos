/* SPDX-License-Identifier: GPL-2.0-only */
#include "a733_usb2_native.h"
#include "tusb.h"
#include "host/hcd.h"
#include <string.h>
static struct a733_usb2_native *host;
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
    host=n;const tusb_rhport_init_t init={.role=TUSB_ROLE_HOST,.speed=TUSB_SPEED_HIGH};
    if(!tusb_init(0,&init)){lost();return 0;}return 1;
}
int a733_usb2_host_poll(struct a733_usb2_native *n)
{
    uint64_t now;if(n!=host||!clock_now(&now)){lost();return -1;}
    if((!host->mounted&&now>=host->platform.config.deadline)||
        (host->read_pending&&now>=host->read_deadline)||
        (host->hid.pending&&now>=host->hid.deadline)){lost();return -1;}
    enum a733_xhci_result r=a733_xhci_poll(&host->core);
    if(r==A733_XHCI_RETAINED||r==A733_XHCI_INVALID){lost();return -1;}
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
static int control_response(const struct a733_xhci_notice *e)
{
    struct a733_usb2_native *n=host;const uint8_t *p=n->control_buffer,*s=n->control_setup;
    unsigned bytes=n->control_bytes;
    if(!p)return 1;
    if(e->endpoint!=128||e->result||e->bytes!=bytes||bytes!=le16(s+6))return 0;
    n->control_buffer=NULL;n->control_bytes=0;
    if(s[0]==0xa1&&s[1]==0xfe)return bytes==1&&p[0]==0;
    if(s[0]==0x81&&s[1]==6&&s[3]==0x22)
        return n->kind==A733_USB2_DEVICE_HID&&bytes==n->hid.descriptor_bytes;
    if(s[0]!=0x80||s[1]!=6||bytes<2||p[1]!=s[3])return 0;
    if(s[3]==1){
        if(p[0]!=18||p[4]||p[5]||p[6]||
           (n->core.speed==2&&p[7]!=8)||(n->core.speed==3&&p[7]!=64)||
           (n->core.speed==1&&p[7]!=8&&p[7]!=16&&p[7]!=32&&p[7]!=64))return 0;
        if(bytes==8){if(n->device_probe)return 0;memcpy(n->device_prefix,p,8);n->device_probe=1;return 1;}
        return bytes==18&&n->device_probe&&!memcmp(n->device_prefix,p,8)&&p[17]==1;
    }
    if(s[3]==2){
        if(bytes<9||p[0]!=9||(le16(p+2)!=32&&le16(p+2)!=34)||p[4]!=1||p[5]!=1||!(p[7]&128)||p[8]>250)return 0;
        if(bytes==9){if(n->configuration_probe)return 0;memcpy(n->configuration_prefix,p,9);n->configuration_probe=1;return 1;}
        if(bytes!=le16(p+2)||!n->configuration_probe||memcmp(n->configuration_prefix,p,9)||
           p[9]!=9||p[10]!=4||p[11]||p[12])return 0;
        if(bytes==34){
            unsigned ep=p[29],packet=le16(p+31),interval=p[33];
            if(p[13]!=1||p[14]!=3||p[15]>1||p[16]>2||(!p[15]&&p[16])||
               (p[15]&&!p[16])||p[18]!=9||p[19]!=0x21||p[23]!=1||p[24]!=0x22||
               !le16(p+25)||le16(p+25)>256||p[27]!=7||p[28]!=5||
               ep&~0x8fu||!(ep&128)||!(ep&15)||p[30]!=3||!packet||
               packet>(n->core.speed==2?8u:64u)||!interval||interval>(n->core.speed==3?16u:255u))return 0;
            n->kind=A733_USB2_DEVICE_HID;n->hid.endpoint=ep;n->hid.packet=packet;n->hid.interval=interval;
            n->hid.subclass=p[15];n->hid.protocol=p[16];n->hid.descriptor_bytes=le16(p+25);return 1;
        }
        if(n->core.speed!=3||p[13]!=2||p[14]!=8||p[15]!=6||p[16]!=0x50)return 0;
        unsigned dirs=0,seen=0;
        for(unsigned at=18;at<32;at+=7){unsigned ep=p[at+2];
            if(p[at]!=7||p[at+1]!=5||(ep&0x70)||!(ep&15)||p[at+3]!=2||le16(p+at+4)!=512)return 0;
            unsigned bit=1u<<((ep&15)+((ep&128)?16:0));if(seen&bit)return 0;seen|=bit;dirs|=(ep&128)?2:1;}
        if(dirs!=3)return 0;n->kind=A733_USB2_DEVICE_MSC;return 1;
    }
    if(s[3]==3){
        if(p[0]<2||(p[0]&1)||(!s[2]&&p[0]<4))return 0;
        if(bytes==2){n->string_bytes=p[0];n->string_id=s[2];n->string_language=le16(s+4);return 1;}
        return bytes==p[0]&&bytes==n->string_bytes&&s[2]==n->string_id&&le16(s+4)==n->string_language&&
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
            if(n->kind==A733_USB2_DEVICE_HID){
                if(!n->hid.pending||n->hid.event_seen||event->endpoint!=n->hid.endpoint||
                   event->address!=n->address||event->result||event->bytes>n->hid.requested){lost();return;}
                n->hid.event_seen=1;n->hid.event_bytes=event->bytes;
            }else if(!bot_complete(event)){lost();return;}
        }else if(!control_response(event)){lost();return;}
        hcd_event_xfer_complete((uint8_t)event->address,(uint8_t)event->endpoint,event->bytes,
            event->result==0?XFER_RESULT_SUCCESS:event->result==1?XFER_RESULT_STALLED:XFER_RESULT_FAILED,false);
    }else lost();
}
bool hcd_init(uint8_t port,const tusb_rhport_init_t *i)
{return owner()&&!port&&i&&i->role==TUSB_ROLE_HOST&&i->speed==TUSB_SPEED_HIGH;}
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
{return owner()&&!port?(host->core.speed==3?TUSB_SPEED_HIGH:host->core.speed==2?TUSB_SPEED_LOW:host->core.speed==1?TUSB_SPEED_FULL:TUSB_SPEED_INVALID):TUSB_SPEED_INVALID;}
void hcd_device_close(uint8_t port,uint8_t address)
{
    /* TinyUSB retires address0 immediately after SET_ADDRESS Status. The same
     * physical slot/EP0 stays live at its confirmed assigned address. */
    if(owner()&&!port&&!address&&!host->address_zero_closed&&host->core.address&&
        host->core.hardware_address&&!host->core.pending_address&&!host->core.address_status&&
        !host->core.control_stage&&!host->core.endpoints[0].pending){host->address_zero_closed=1;return;}
    lost();
}
bool hcd_edpt_open(uint8_t port,uint8_t address,const tusb_desc_endpoint_t *e)
{
    uint64_t d=deadline();if(port||!e||!d)return false;
    if(e->bmAttributes.xfer==TUSB_XFER_INTERRUPT)
        return host->kind==A733_USB2_DEVICE_HID&&e->bEndpointAddress==host->hid.endpoint&&
          e->bmAttributes.xfer==3&&tu_unaligned_read16(&e->wMaxPacketSize)==host->hid.packet&&
          e->bInterval==host->hid.interval&&a733_xhci_open_interrupt(&host->core,address,
             e->bEndpointAddress,host->hid.packet,e->bInterval,d);
    return a733_xhci_open(&host->core,address,e->bEndpointAddress,e->bmAttributes.xfer,tu_edpt_packet_size(e),d);
}
bool hcd_edpt_close(uint8_t port,uint8_t address,uint8_t ep)
{(void)port;(void)address;(void)ep;lost();return false;}
bool hcd_setup_send(uint8_t port,uint8_t address,const uint8_t setup[8])
{
    uint64_t d=deadline();if(port||!d||!setup||host->control_buffer)return false;
    unsigned len=le16(setup+6),value=le16(setup+2),index=le16(setup+4);int valid=0;
    if(setup[0]==0x80&&setup[1]==6){
        valid=(setup[3]==1&&!setup[2]&&!index&&(len==8||len==18))||
            (setup[3]==2&&!setup[2]&&!index&&(len==9||(host->configuration_probe&&len==le16(host->configuration_prefix+2))))||
            (setup[3]==3&&len>=2&&len<=254&&!(len&1));
    }else if(!setup[0]&&!index&&!len)valid=(setup[1]==5&&value&&value<=127)||(setup[1]==9&&value==1);
    else if(host->kind==A733_USB2_DEVICE_HID&&!index){
        valid=(setup[0]==0x81&&setup[1]==6&&value==0x2200&&len==host->hid.descriptor_bytes)||
          (setup[0]==0x21&&!len&&!value&&(setup[1]==0x0a||(setup[1]==0x0b&&host->hid.subclass==1)));
    }else valid=setup[0]==0xa1&&setup[1]==0xfe&&!value&&!index&&len==1;
    if(!valid){lost();return false;}
    if(!a733_xhci_setup(&host->core,address,setup,d))return false;
    memcpy(host->control_setup,setup,8);return true;
}
bool hcd_edpt_xfer(uint8_t port,uint8_t address,uint8_t ep,uint8_t *buffer,uint16_t bytes)
{
    uint64_t d=deadline();if(port||!d)return false;
    if(ep&15){
        if(host->kind==A733_USB2_DEVICE_HID){
            if(!host->hid.pending||host->hid.pending_buffer||ep!=host->hid.endpoint||bytes!=host->hid.packet||!buffer){lost();return false;}
            host->hid.pending_buffer=buffer;host->hid.requested=bytes;
        }else if(!bot_submit(ep,buffer,bytes)){lost();return false;}
    }
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
    if(!owner()||host->kind!=A733_USB2_DEVICE_MSC||host->mounted||host->bot_phase||host->bot_stage){lost();return;}
    host->blocks=tuh_msc_get_block_count(address,0);host->block_bytes=tuh_msc_get_block_size(address,0);
    if(!host->blocks||host->block_bytes!=512||host->bot_status){lost();return;}
    host->address=address;host->mounted=1;
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
    uint64_t now;if(n!=host||!clock_now(&now)||!n->mounted||n->kind!=A733_USB2_DEVICE_MSC||n->read_pending||n->read_complete||
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
    if(!owner()||host->kind!=A733_USB2_DEVICE_HID||host->mounted||instance||
       address!=host->core.address||!descriptor||bytes!=host->hid.descriptor_bytes||
       !bytes||bytes>sizeof(host->hid.descriptor)||!clock_now(&now)||
       host->core.ops.live(host->core.context,host->core.generation,host->platform.config.deadline)!=1){lost();return;}
    memcpy(host->hid.descriptor,descriptor,bytes);host->address=address;host->mounted=1;
}
void tuh_hid_umount_cb(uint8_t address,uint8_t instance)
{(void)address;(void)instance;lost();}
void tuh_hid_report_received_cb(uint8_t address,uint8_t instance,const uint8_t *report,uint16_t bytes)
{
    if(!owner()){lost();return;}
    uint64_t now;struct a733_usb2_hid *h=&host->hid;
    /* Pinned TinyUSB ignores result in hidh_xfer_cb. Only the HCD's validated
     * successful event may reach this callback, including actual short/ZLP. */
    if(!owner()||host->kind!=A733_USB2_DEVICE_HID||address!=host->address||instance||
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
    if(n!=host||!clock_now(&now)||!n->mounted||n->kind!=A733_USB2_DEVICE_HID||
       n->hid.pending||n->hid.complete||d<=now||d>n->platform.config.lifetime_deadline)return 0;
    if(n->core.ops.live(n->core.context,n->core.generation,d)!=1)return 0;
    n->hid.deadline=d;n->hid.pending=1;n->hid.event_seen=0;n->hid.event_bytes=0;
    if(!tuh_hid_receive_report((uint8_t)n->address,0)){lost();return 0;}return 1;
}
int a733_usb2_hid_result(struct a733_usb2_native *n,struct a733_usb2_hid_report *out)
{
    uint64_t now;
    if(n!=host||!clock_now(&now)||n->kind!=A733_USB2_DEVICE_HID)return -1;
    if(!n->hid.pending||!n->hid.complete)return 0;
    if(now>=n->hid.deadline){lost();return -1;}
    if(!out||!a733_usb2_platform_outside(&n->platform,out,sizeof(*out)))return -1;
    int live=n->core.ops.live(n->core.context,n->core.generation,n->hid.deadline);
    if(live<0){lost();return -1;}if(!live)return 0;
    struct a733_usb2_hid_report value={.generation=n->core.generation,.sequence=n->hid.sequence,
      .bytes=n->hid.event_bytes,.address=(uint8_t)n->address,.interface=0,.protocol=(uint8_t)n->hid.protocol,
      .endpoint=(uint8_t)n->hid.endpoint,.speed=(uint8_t)n->core.speed};
    memcpy(value.data,n->hid.copied,value.bytes);
    if(!clock_now(&now)||now>=n->hid.deadline||
       n->core.ops.live(n->core.context,n->core.generation,n->hid.deadline)!=1){lost();return -1;}
    memcpy(out,&value,sizeof(value));n->hid.pending=n->hid.complete=n->hid.event_seen=0;return 1;
}
