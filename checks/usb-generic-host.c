/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>
#include "a733_usb_memory.h"
static void generic_control_data(uint8_t *,unsigned);
static void generic_wire(void);
#define main legacy_fixture_main
#include "usb-generic-base.c"
#undef main
#include "sunstone_usb_service.h"
#include "sunstone_usb_hid.h"
#include "sunstone_usb_serial.h"
#include "sunstone_usb_uvc.h"
#include "usb-client-write.inc"
#if SUNSTONE_USB_APP_ALIAS
#define MODEL_PHYSICAL A733_USB_MEMORY_BASE
#else
#define MODEL_PHYSICAL UINT64_C(0x79000000)
#endif

/* These inherited helpers belong to the legacy main, which is not run. */
void usb2_native_mapping_check(void){assert(0);}
void usb2_source_reference(uint32_t *p){(void)p;assert(0);}
static struct ss_grant application_grant;
const struct ss_grant *ss_grant(void){return &application_grant;}
int ss_live(void){return 1;}
static unsigned fixture_class,inputs,packets,payload_bytes,control_count,serial_echoes;
static unsigned direct_write,gate_held,gate_busy;
static uint64_t tx_ready_at,completed_tx;
static unsigned completed_dci,completed_slot;
static uint64_t received_pa;
static unsigned frame_sent,bulk_sent,hold_data;
static uint64_t seen_iso[A733_XHCI_ISOCH_TDS];
static uint8_t configuration[4096],probe[48];
static unsigned configuration_bytes;
static const uint8_t keyboard_descriptor[]={
  5,1,9,6,0xa1,1,5,7,0x19,0xe0,0x29,0xe7,0x15,0,0x25,1,
  0x75,1,0x95,8,0x81,2,0x95,1,0x75,8,0x81,1,0x95,6,0x75,8,
  0x15,0,0x25,0x65,5,7,0x19,0,0x29,0x65,0x81,0,0xc0};
static void append(const uint8_t *p,unsigned bytes)
{assert(configuration_bytes+bytes<=sizeof(configuration));memcpy(configuration+configuration_bytes,p,bytes);configuration_bytes+=bytes;}
static void make_configuration(void)
{
    const uint8_t config[]={9,2,0,0,1,1,0,0x80,50};append(config,sizeof(config));
    if(fixture_class==1){
        const uint8_t hid[]={9,4,0,0,1,3,1,1,0,9,0x21,0x11,1,0,1,0x22,sizeof(keyboard_descriptor),0,7,5,0x81,3,8,0,4};
        append(hid,sizeof(hid));
    }else if(fixture_class==2){
        /* Public CP2102 RPLIDAR interface facts; see usb-serial-sources.json. */
        const uint8_t serial[]={9,4,0,0,2,255,0,0,0,7,5,0x81,2,64,0,0,7,5,1,2,64,0,0};
        append(serial,sizeof(serial));
    }else{
        configuration[4]=2;
        const uint8_t vc[]={9,4,0,0,0,14,1,0,0,13,0x24,1,0x10,1,13,0,0,0,0,0,1,1};
        const uint8_t vs[]={9,4,1,0,0,14,2,0,0};
        const uint8_t format[]={27,0x24,4,1,1,'Y','U','Y','2',0,0,0x10,0,0x80,0,0,0xaa,0,0x38,0x9b,0x71,16,1,0,0,0,0};
        uint8_t frame[30]={30,0x24,5,1,0,0x80,2,0xe0,1};
        put32(frame+9,147456000);put32(frame+13,147456000);
        put32(frame+17,614400);put32(frame+21,333333);frame[25]=1;put32(frame+26,333333);
        uint8_t endpoint[]={9,4,1,1,1,14,2,0,0,7,5,0x81,1,0,0x14,1};
        if(fixture_class==4){endpoint[3]=0;endpoint[12]=2;endpoint[13]=0;endpoint[14]=2;}
        append(vc,sizeof(vc));append(vs,sizeof(vs));
        if(fixture_class==5){
            const uint8_t mjpeg[]={11,0x24,6,1,1,1,1,0,0,0,0};
            frame[2]=7;append(mjpeg,sizeof(mjpeg));
        }else append(format,sizeof(format));
        append(frame,sizeof(frame));
        if(fixture_class==4){
            /* Keep descriptor order: remove the final alternate-zero interface
             * by rebuilding the stream section, preserving its class facts. */
            configuration_bytes=9+sizeof(vc);append(endpoint,9);append(format,sizeof(format));append(frame,sizeof(frame));append(endpoint+9,7);
        }else append(endpoint,sizeof(endpoint));
    }
    configuration[2]=(uint8_t)configuration_bytes;configuration[3]=(uint8_t)(configuration_bytes>>8);
}
static void generic_control_data(uint8_t *p,unsigned count)
{
    memset(p,0,count);
    if(request[1]==6){
        uint8_t dev[]={18,1,0,2,0,0,0,64,0x34,0x12,0x78,0x56,0,1,0,0,0,1};
        if(fixture_class==2){dev[8]=0xc4;dev[9]=0x10;dev[10]=0x60;dev[11]=0xea;}
        const uint8_t language[]={4,3,9,4};
        const uint8_t *src=request[3]==1?dev:request[3]==2?configuration:request[3]==0x22?keyboard_descriptor:language;
        unsigned size=request[3]==1?sizeof(dev):request[3]==2?configuration_bytes:request[3]==0x22?sizeof(keyboard_descriptor):sizeof(language);
        assert(count<=size);memcpy(p,src,count);
    }else if(fixture_class>=3&&request[0]==0xa1&&request[1]==0x81){
        assert(count==34);memcpy(p,probe,count);put32(p+18,614400);put32(p+22,fixture_class==4?4096:3072);
    }else if(fixture_class==2){
        /* CP210x's initialization reads flow control and part identity. */
        if(request[1]==0xff&&count)p[0]=2;
    }else assert(0);
}
static unsigned camera_payload(uint8_t *p,unsigned capacity)
{
    unsigned total=fixture_class==5?4:614400;
    assert(capacity>=2);unsigned left=total-payload_bytes,bytes=capacity-2;
    if(bytes>left)bytes=left;p[0]=2;p[1]=(uint8_t)(bytes==left?2:0);
    for(unsigned i=0;i<bytes;++i)p[2+i]=(uint8_t)((payload_bytes+i)*17+3);
    payload_bytes+=bytes;++packets;if(payload_bytes==total)frame_sent=1;return bytes+2;
}
static void generic_wire(void)
{
    struct a733_xhci *x=&n.core;
    if(x->command_kind&&x->command_pointer!=last_command){
        unsigned j=x->command_device-1;assert(j<A733_XHCI_DEVICES);struct a733_xhci_device *d=&x->devices[j];
        if(d->detach&&(x->command_kind==10||x->command_kind==14||x->command_kind==15)){teardown_command(j);return;}
        last_command=x->command_pointer;++commands;
        if(x->command_kind==11){
            memcpy(d->device,d->input+x->context_bytes,2*x->context_bytes);running(d->device+x->context_bytes);
            put32(d->device+12,((d->pending_address?2u:1u)<<27)|(d->pending_address?7u:0));
        }else if(x->command_kind==12){
            memcpy(d->device,d->input+x->context_bytes,x->context_bytes);put32(d->device+12,(3u<<27)|7);
            for(unsigned i=2;i<32;++i)if(get32(d->input+4)&(1u<<i)){
                memcpy(d->device+i*x->context_bytes,d->input+(i+1)*x->context_bytes,x->context_bytes);running(d->device+i*x->context_bytes);}
        }else if(x->command_kind==13){
            memcpy(d->device+x->context_bytes,d->input+2*x->context_bytes,x->context_bytes);running(d->device+x->context_bytes);
        }else assert(x->command_kind==9);
        event(33,x->command_pointer,1,0,0,j+1);return;
    }
    if((fixture_class==3||fixture_class==5)&&x->isoch.count&&!frame_sent&&!hold_data){
        struct a733_xhci_isoch *s=&x->isoch;unsigned k=(s->head+s->done)%A733_XHCI_ISOCH_TDS;
        if(s->state[k]==1&&seen_iso[k]!=s->trb[k]){
            seen_iso[k]=s->trb[k];
            uint8_t *p=dma+A733_XHCI_DATA_OFFSET+s->buffer[k]*4096;
            struct a733_xhci_device *d=&x->devices[s->device-1];
            const struct a733_xhci_endpoint *e=&d->endpoints[1];
            const struct a733_xhci_trb *td=&e->ring[(s->trb[k]-e->ring_pa)/16];
            unsigned requested=td->status&0x1ffff,count=camera_payload(p,requested);
            event(32,s->trb[k],count==requested?1:13,requested-count,
                  d->endpoints[1].number,d->slot);return;
        }
    }
    for(unsigned j=0;j<A733_XHCI_DEVICES;++j)for(unsigned i=0;i<A733_XHCI_ENDPOINTS;++i){
        struct a733_xhci_device *d=&x->devices[j];struct a733_xhci_endpoint *e=&d->endpoints[i];
        if(e->pending!=2||!e->pending_trb||e->pending_trb==last_transfer[j][i])continue;
        if(i&&(hold_data||(fixture_class==1&&hid_reports)||(fixture_class==2&&bulk_sent&&e->direction)||(fixture_class==4&&frame_sent)))continue;
        last_transfer[j][i]=e->pending_trb;++transfers;unsigned actual=e->length;
        if(!i){
            if(e->kind==2){memcpy(request,d->setup,8);++control_count;}
            else if(e->kind==3&&e->direction)generic_control_data(e->bounce,e->length);
            else if(e->kind==3&&!e->direction&&fixture_class>=3&&request[0]==0x21&&request[1]==1){assert(e->length==34);memcpy(probe,e->bounce,34);}
        }else{
            const struct a733_xhci_trb *td=&e->ring[(e->pending_trb-e->ring_pa)/16];
            uint64_t destination=td->parameter_lo|((uint64_t)td->parameter_hi<<32);
            assert(destination>=MODEL_PHYSICAL&&destination+e->length<=MODEL_PHYSICAL+sizeof(dma));
            uint8_t *p=dma+(destination-MODEL_PHYSICAL);
            if(fixture_class==1){assert(e->length==8);memset(p,0,8);p[2]=4;++hid_reports;}
            else if(fixture_class==2&&e->direction){actual=64;for(unsigned k=0;k<actual;++k)p[k]=(uint8_t)(k*17+3);received_pa=destination;++bulk_sent;}
            else if(fixture_class==2){
                if(tx_ready_at&&m.now<tx_ready_at){last_transfer[j][i]=0;return;}
                assert(destination==received_pa&&e->length==64);for(unsigned k=0;k<64;++k)assert(p[k]==(uint8_t)(k*17+3));++serial_echoes;
                completed_tx=e->pending_trb;completed_dci=e->number;completed_slot=d->slot;
            }
            else if(fixture_class==4)actual=camera_payload(p,e->length);
            else assert(0);
        }
        event(32,e->pending_trb,actual==e->length?1:13,e->length-actual,e->number,d->slot);return;
    }
}
static void input(void *context,const struct sunstone_hid_event *e)
{(void)context;if(e->type==SUNSTONE_HID_KEY){assert(e->usage==4&&e->value==1);++inputs;}}
static void released(void *context,unsigned device,uint64_t token)
{(void)context;assert(device==SS_DEVICE_USB_SERIAL);os_usb_serial_native_release(token);}
static int write_serial(void *context,uint64_t sequence,const void *data,size_t bytes)
{if(direct_write)assert(gate_held);return usb_write(context,sequence,data,bytes);}
extern uint32_t a733_tinyusb_next_timeout_ms(uint32_t);
static int client_enter(void *context)
{(void)context;assert(!gate_held);if(gate_busy)return 0;gate_held=1;return 1;}
static void client_leave(void *context)
{(void)context;assert(gate_held);gate_held=0;}
int main(int argc,char **argv)
{
#if SUNSTONE_USB_APP_ALIAS
    int fd=memfd_create("sunstone-usb-model",MFD_CLOEXEC);assert(fd>=0);
    assert(ftruncate(fd,A733_XHCI_MEMORY_BYTES)==0);
    dma_pages=mmap((void *)A733_USB_MEMORY_BASE,A733_XHCI_MEMORY_BYTES,PROT_READ|PROT_WRITE,
                   MAP_SHARED|MAP_FIXED_NOREPLACE,fd,0);
    assert((void *)dma_pages==(void *)A733_USB_MEMORY_BASE);
    void *alias=mmap((void *)A733_USB_MEMORY_APP_START,A733_USB_MEMORY_DATA_BYTES,PROT_READ,
                    MAP_SHARED|MAP_FIXED_NOREPLACE,fd,A733_USB_MEMORY_DATA_OFFSET);
    assert(alias==(void *)A733_USB_MEMORY_APP_START);
#endif
    assert(argc==2);fixture_class=!strcmp(argv[1],"hid")?1:!strncmp(argv[1],"serial",6)?2:!strcmp(argv[1],"iso")?3:!strcmp(argv[1],"bulk")?4:!strcmp(argv[1],"mjpeg")?5:6;
    direct_write=!strcmp(argv[1],"serial-direct")||!strcmp(argv[1],"serial-stale");
    application_grant.devices=SS_DEVICE_USB_KEYBOARD|SS_DEVICE_USB_MOUSE|SS_DEVICE_USB_SERIAL|SS_DEVICE_USB_CAMERA;
    const struct os_usb_service_ops ops={.release=released,.write=write_serial,
        .enter=direct_write?client_enter:NULL,.leave=direct_write?client_leave:NULL};os_usb_service_init(&ops,NULL);
    make_configuration();device_speed=fixture_class==2?1:3;irq_model=1;setup();prepare();assert(a733_usb2_generic_enable(&n));
    a733_usb2_set_input(&n,input,NULL);start_core();assert(os_usb_serial_native_start(a733_usb2_transport(&n))==1);
    unsigned deferred_timers=0,ready_batches=0;
    for(unsigned k=0;k<18000&&!tuh_mounted(1)&&!n.failed;++k){
        poll();
        if(!n.failed){
            uint64_t next=a733_usb2_host_deadline(&n);assert(next&&next<=n.platform.config.lifetime_deadline);
            ready_batches+=a733_usb2_host_work_pending(&n)!=0;
            uint32_t ms=a733_tinyusb_next_timeout_ms((uint32_t)(m.now/1000));
            if(ms!=UINT32_MAX){uint64_t at=m.now-m.now%1000+(uint64_t)ms*1000;
                if(at<m.now)at=m.now;
                assert(next<=at);++deferred_timers;
            }
        }
    }
    if(n.failed||!tuh_mounted(1))fprintf(stderr,"enumeration failed class%u phase%u controls%u time%llu\n",fixture_class,n.core.phase,control_count,(unsigned long long)m.now);
    assert(!n.failed&&tuh_mounted(1));
    assert(deferred_timers&&ready_batches); /* Removing either timer or ready export fails. */
    if(fixture_class==1){
        for(unsigned k=0;k<10000&&!inputs&&!n.failed;++k)poll();assert(inputs==1&&!n.failed);
        puts("PASS generic TinyUSB HID enumeration, report parsing and IRQ completion");
    }else if(fixture_class==2){
        struct ss_usb_serial_buffer b;int got=0;
        for(unsigned k=0;k<10000&&!got&&!n.failed;++k){poll();os_usb_service_dispatch();got=ss_usb_serial_acquire(&b);}
        if(n.failed||got!=1)fprintf(stderr,"serial failed result%d failed%u controls%u packets%u phase%u time%llu\n",got,n.failed,control_count,bulk_sent,n.core.phase,(unsigned long long)m.now);
        assert(!n.failed&&got==1&&b.bytes==64);
#if SUNSTONE_USB_APP_ALIAS
        assert((uintptr_t)b.data>=A733_USB_MEMORY_APP_START&&(uintptr_t)b.data+64<=A733_USB_MEMORY_APP_END);
#else
        assert(b.data>=dma&&b.data<dma+sizeof(dma));
#endif
        for(unsigned k=0;k<64;++k)assert(b.data[k]==(uint8_t)(k*17+3));
        if(direct_write){gate_busy=1;assert(ss_usb_serial_write(1,b.data,b.bytes)==0);gate_busy=0;tx_ready_at=m.now+3500;}
        assert(ss_usb_serial_write(1,b.data,b.bytes)==1);
        if(direct_write){
            assert(!gate_held);unsigned on_ring=0;
            for(unsigned i=1;i<A733_XHCI_ENDPOINTS;++i){struct a733_xhci_endpoint *e=&n.core.devices[0].endpoints[i];
                if(e->address==1&&e->pending==2)on_ring=1;}
            assert(on_ring); /* No dispatch or owner poll between caller and doorbell. */
        }
        os_usb_service_dispatch();size_t actual=0;int sent=0;
        if(direct_write)for(unsigned k=0;m.now<tx_ready_at&&k<1000;++k){
            assert(ss_usb_serial_write_result(1,&actual)==0);poll();}
        for(unsigned k=0;k<10000&&!sent&&!n.failed;++k){poll();os_usb_service_dispatch();sent=ss_usb_serial_write_result(1,&actual);}
        assert(sent==1&&actual==64&&serial_echoes==1&&!n.failed);
        if(!strcmp(argv[1],"serial-stale")){
            /* Replay the hardware record only after its real callback. It must
             * retain the session, never complete another ticket or reuse DMA. */
            event(32,completed_tx,1,0,completed_dci,completed_slot);poll();
            assert(n.failed&&serial_echoes==1);puts("PASS stale direct TX record retains after delayed matched completion");return 0;
        }
        hold_data=1;registers[0x440/4]=(1u<<17);event(34,UINT64_C(1)<<24,1,0,0,0);
        for(unsigned k=0;k<10000&&(!removals||!disables)&&!n.failed;++k){poll();os_usb_service_dispatch();}
        assert(removals==1&&disables==1&&!n.failed);
        for(unsigned k=0;k<64;++k)assert(b.data[k]==(uint8_t)(k*17+3));
        assert(ss_usb_serial_release(b.token)==1);os_usb_service_dispatch();
        for(unsigned k=0;k<10000&&(n.core.devices[0].slot||n.core.announced||n.core.detaching)&&!n.failed;++k)poll();
        assert(!n.failed&&!n.core.devices[0].slot&&!n.core.announced&&!n.core.detaching);
        hold_data=0;bulk_sent=0;registers[0x440/4]=1|(1u<<17);event(34,UINT64_C(1)<<24,1,0,0,0);
        for(unsigned k=0;k<18000&&!tuh_mounted(1)&&!n.failed;++k)poll();assert(tuh_mounted(1)&&!n.failed);
        for(unsigned k=0;k<10000&&!tuh_cdc_mounted(0)&&!n.failed;++k)poll();assert(tuh_cdc_mounted(0)&&!n.failed);
        got=0;for(unsigned k=0;k<10000&&!got&&!n.failed;++k){poll();os_usb_service_dispatch();got=ss_usb_serial_acquire(&b);}
        assert(got==1&&b.bytes==64&&!n.failed);assert(ss_usb_serial_release(b.token)==1);os_usb_service_dispatch();
        puts("PASS generic TinyUSB CP2102 zero-copy app echo, held detach lease and reattachment");
    }else{
        if(fixture_class==5||fixture_class==6)assert(a733_usb2_camera_select_format(&n,SUNSTONE_UVC_MJPEG));
        if(fixture_class==6){assert(a733_usb2_camera_start(&n)==-1&&!n.failed);puts("PASS explicit MJPEG selection refuses an unsupported encoding");return 0;}
        assert(a733_usb2_camera_start(&n)==1);const struct sunstone_uvc_frame *f=NULL;
        if(fixture_class==5)assert(a733_usb2_camera_select_format(&n,SUNSTONE_UVC_MJPEG));
        for(unsigned k=0;k<20000&&!f&&!n.failed;++k){poll();f=a733_usb2_camera_acquire(&n);}
        if(!f||n.failed)fprintf(stderr,"camera failed class%u phase%u controls%u packets%u bytes%u time%llu\n",fixture_class,n.core.phase,control_count,packets,payload_bytes,(unsigned long long)m.now);
        unsigned total=fixture_class==5?4:614400;
        assert(!n.failed&&f&&f->bytes==total);unsigned count=0;
        for(unsigned i=0;i<f->slices;++i){assert(f->slice[i].data>=dma&&f->slice[i].data<dma+sizeof(dma));
            for(size_t k=0;k<f->slice[i].bytes;++k)assert(f->slice[i].data[k]==(uint8_t)(count++*17+3));}
        assert(count==total&&a733_usb2_camera_release(&n,f,f->generation,f->sequence)==1);
        puts("PASS generic TinyUSB UVC probe/commit and zero-copy VGA YUYV frame");
    }
    assert(irq_begins&&irq_releases);return 0;
}
