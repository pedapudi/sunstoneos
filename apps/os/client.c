/* USB2 owner and the application's single-sector/read-keyboard mailboxes. */
#include "os.h"
#include "application.h"
#include "cpu-return.h"
#include "quiesce.h"
#include "../board/application.h"
#include "../board/session.h"
#include "sunstone_crumb.h"
#ifdef SUNSTONE_BOARD_USB2_IRQ
#include "../board/host_irq.h"
#endif
#include <string.h>
static enum os_quiesce_result usb_quiesce_step(struct os_quiesce *);
static struct os_quiesce usb_quiesce={
    .owner=OS_QUIESCE_USB,.order=OS_QUIESCE_DEVICES,.step=usb_quiesce_step
};
/* Called before the board creates its host, Type-C and power tasks. */
int os_usb_quiesce_start(void)
{return !os_returning()||os_quiesce_register(&usb_quiesce);}
static enum os_quiesce_result usb_quiesce_step(struct os_quiesce *q)
{
    if(os_quiesce_result(q)==OS_QUIESCE_DONE)return OS_QUIESCE_DONE;
    if(board.handles[4])xTaskNotifyGive(board.handles[4]);
    return os_quiesce_result(q);
}
static void usb_fail_at(uint32_t site,uint32_t value,const void *caller)
{
    if(os_quiesce_fault(&usb_quiesce,site,value,caller))return;
    board_fail_at(site,value,caller);
}
#define usb_fail() usb_fail_at(SUNSTONE_CRUMB_FAIL_SITE(BOARD),0,SUNSTONE_CRUMB_HERE())
/* Used after the owner stopped accepting work. A lost controller remains
 * retained; neither an empty port nor a masked IRQ certifies DMA completion. */
static void usb_quiesce_unproven(unsigned core_started)
{usb_fail_at(SUNSTONE_CRUMB_FAIL_BOARD<<12|0x801u,core_started?2u:1u,SUNSTONE_CRUMB_HERE());}
void os_application_usb_changed(void);
#ifdef SUNSTONE_A733_OS_USB_JPEG
#include "sunstone_usb_service.h"
#include "sunstone_usb_jpeg.h"
#include "../jpeg/a733_jpeg_application.h"
#include "a733_usb_memory.h"
static struct {atomic_uint state;uint64_t token;struct ss_usb_decoded_frame view;int result;} decode;
int ss_usb_camera_decode(uint64_t token,struct ss_usb_decoded_frame *view)
{
    const struct ss_grant *g=ss_grant();
    if(!token||!view||!g||!(g->devices&SS_DEVICE_USB_CAMERA)||ss_live()<0)return -1;
    unsigned state=atomic_load_explicit(&decode.state,memory_order_acquire);
    if(!state){decode.token=token;atomic_store_explicit(&decode.state,1,memory_order_release);
        if(board.handles[4])xTaskNotifyGive(board.handles[4]);return 0;}
    if(token!=decode.token)return -1;
    if(state!=3)return 0;if(decode.result==1)*view=decode.view;return decode.result;
}
static void decode_poll(void)
{
    unsigned state=atomic_load_explicit(&decode.state,memory_order_acquire);
    if(state==1){
        struct ss_usb_camera_frame frame;
        if(!os_usb_service_camera_pin(decode.token,&frame)){decode.result=-1;atomic_store_explicit(&decode.state,3,memory_order_release);void os_application_usb_changed(void);os_application_usb_changed();return;}
        static struct ss_usb_jpeg_slice slices[1024];
        static struct a733_jpeg_session session;
        size_t bytes=0;const void *raw=os_session_extent(board_session_sealed(),SS_EXTENT_JPEG,&bytes);
        int r=0;
        if(raw&&bytes==sizeof(session)&&frame.format==SS_USB_FRAME_MJPEG&&frame.slice_count<=1024){
            memcpy(&session,raw,sizeof(session));session.board=board.session;
            for(size_t i=0;i<frame.slice_count;++i)slices[i]=(struct ss_usb_jpeg_slice){a733_usb_memory_from_app(frame.slices[i].data,frame.slices[i].bytes),frame.slices[i].bytes};
            const struct ss_usb_jpeg_frame input={frame.token,slices,frame.slice_count,frame.bytes,frame.width,frame.height};
            r=ss_usb_jpeg_native_begin(&session,&input);
        }
        os_usb_service_camera_unpin(decode.token);
        if(r!=1){decode.result=-1;atomic_store_explicit(&decode.state,3,memory_order_release);void os_application_usb_changed(void);os_application_usb_changed();return;}
        atomic_store_explicit(&decode.state,2,memory_order_release);state=2;
    }
    if(state==2){struct ss_usb_jpeg_view view;int r=ss_usb_jpeg_native_poll(&view);
        if(r){decode.result=r;if(r==1)decode.view=(struct ss_usb_decoded_frame){view.data,view.bytes,view.width,view.height};
            atomic_store_explicit(&decode.state,3,memory_order_release);
            void os_application_usb_changed(void);os_application_usb_changed();}
    }
}
#else
int ss_usb_camera_decode(uint64_t token,struct ss_usb_decoded_frame *view)
{(void)token;(void)view;return -1;}
#endif
#ifdef SUNSTONE_A733_OS_USB_CLASSES
#include "sunstone_usb_service.h"
#include "sunstone_usb_hid.h"
#include "sunstone_usb_transport.h"
#include "a733_usb_memory.h"
#include "sunstone_usb_uvc.h"
#include "sunstone_uvc_camera.h"
int os_usb_serial_native_start(struct sunstone_usb_transport *);
void os_usb_serial_native_release(uint64_t);
int os_usb_serial_native_write(uint64_t,const void *,size_t);
int os_usb_serial_native_configure(const struct ss_usb_serial_line *);
int os_usb_serial_native_configured(void);
static int usb_configure(void *unused,const struct ss_usb_serial_line *line)
{(void)unused;return os_usb_serial_native_configure(line);}
static int usb_configured(void *unused)
{(void)unused;return os_usb_serial_native_configured();}
static void os_usb_camera_native_release(uint64_t);
static int camera_started,camera_connected;
static uint64_t camera_token;
static struct camera_lease {
    const struct sunstone_uvc_frame *frame;
    uint64_t token,generation,sequence;
    struct ss_usb_frame_slice slice[SUNSTONE_UVC_SLICES];
} camera_lease[SUNSTONE_UVC_FRAMES];
static void camera_poll(void)
{
    if(!(os_session_devices()&SS_DEVICE_USB_CAMERA))return;
    if(!camera_started){
#if SUNSTONE_A733_OS_USB_JPEG
        if(!a733_usb2_camera_select_format(&board.host,SUNSTONE_UVC_MJPEG))return;
#endif
        int r=a733_usb2_camera_start(&board.host);if(r<0){usb_fail();return;}camera_started=r;
    }
    const struct sunstone_uvc_mode *mode=a733_usb2_camera_mode(&board.host);
    if(!mode){if(camera_connected)os_usb_service_disconnected(SS_DEVICE_USB_CAMERA);camera_connected=0;camera_started=0;return;}
    struct sunstone_camera_mode camera_mode;
    if(sunstone_uvc_camera_mode(mode,&camera_mode)!=1){usb_fail();return;}
    if(!camera_connected){os_usb_service_connected(SS_DEVICE_USB_CAMERA);camera_connected=1;}
    for(unsigned slot=0;slot<SUNSTONE_UVC_FRAMES;++slot)if(!camera_lease[slot].frame){
        const struct sunstone_uvc_frame *frame=a733_usb2_camera_acquire(&board.host);if(!frame)return;
        struct camera_lease *lease=&camera_lease[slot];
        lease->frame=frame;lease->generation=frame->generation;lease->sequence=frame->sequence;lease->token=++camera_token;
        struct sunstone_camera_frame shared;
        if(sunstone_uvc_camera_view(frame,mode,lease->token,&shared)!=1){
            os_usb_camera_native_release(lease->token);usb_fail();return;
        }
        for(size_t i=0;i<shared.span_count;++i)lease->slice[i]=(struct ss_usb_frame_slice){shared.spans[i].data,shared.spans[i].bytes};
        struct ss_usb_camera_frame view={.token=lease->token,.sequence=shared.sequence,.slices=lease->slice,
            .slice_count=shared.span_count,.bytes=shared.bytes[0],.width=camera_mode.width,.height=camera_mode.height,
            .format=camera_mode.format==SUNSTONE_CAMERA_YUYV?SS_USB_FRAME_YUYV:SS_USB_FRAME_MJPEG};
        if(os_usb_service_frame(&view)!=1){os_usb_camera_native_release(lease->token);return;}
    }
}
static void os_usb_camera_native_release(uint64_t token)
{
    for(unsigned i=0;i<SUNSTONE_UVC_FRAMES;++i){struct camera_lease *lease=&camera_lease[i];
        if(lease->frame&&lease->token==token){
            if(a733_usb2_camera_release(&board.host,lease->frame,lease->generation,lease->sequence)!=1)usb_fail();
            else lease->frame=NULL;return;
        }
    }
    usb_fail();
}
static void usb_changed(void *unused)
{(void)unused;void os_application_usb_changed(void);os_application_usb_changed();}
static void usb_request_wake(void *unused)
{(void)unused;if(board.handles[4])xTaskNotifyGive(board.handles[4]);}
static void usb_release(void *unused,unsigned device,uint64_t token)
{(void)unused;if(device==SS_DEVICE_USB_SERIAL)os_usb_serial_native_release(token);
 else if(device==SS_DEVICE_USB_CAMERA)os_usb_camera_native_release(token);}
static int usb_write(void *unused,uint64_t sequence,const void *data,size_t bytes)
{
    (void)unused;
#ifdef SUNSTONE_USB_APP_ALIAS
    void *native=a733_usb_memory_from_app(data,bytes);if(native)data=native;
#endif
    return os_usb_serial_native_write(sequence,data,bytes);
}
static void usb_input(void *unused,const struct sunstone_hid_event *e);
#endif
static struct {atomic_uint state;uint64_t sector;uint8_t data[512];} storage;
/* ready: empty, published, reader copying, owner publishing. */
static struct {atomic_uint ready;struct ss_key_report report;} keyboard;
#ifdef SUNSTONE_A733_OS_USB_CLASSES
/* The first unread snapshot stays published; later changes coalesce into one
 * pending snapshot. Only the USB owner changes pending and retries publication
 * from serve(), so an application copying a report never delays that owner. */
static struct {struct ss_key_report report;uint64_t attachment;unsigned dirty,invalidate;} keyboard_pending;
static void keyboard_publish(void)
{
    if(!keyboard_pending.dirty&&!keyboard_pending.invalidate)return;
    unsigned state=atomic_load_explicit(&keyboard.ready,memory_order_acquire);
    if(state>1||(state==1&&!keyboard_pending.invalidate)||
       !atomic_compare_exchange_strong_explicit(&keyboard.ready,&state,3,
        memory_order_acq_rel,memory_order_acquire))return;
    if(keyboard_pending.invalidate){state=0;keyboard_pending.invalidate=0;}
    int published=0;
    if(!state&&keyboard_pending.dirty){
        keyboard.report=keyboard_pending.report;
        keyboard_pending.attachment=keyboard.report.attachment;
        keyboard_pending.dirty=0;state=1;published=1;
    }
    atomic_store_explicit(&keyboard.ready,state,memory_order_release);
    if(published)os_application_usb_changed();
}
#endif
int ss_storage_read(uint64_t sector,uint8_t data[512])
{
    const struct ss_grant *g=ss_grant();
    if(!data||sector>UINT32_MAX||!g||!(g->devices&SS_DEVICE_USB_STORAGE)||ss_live()<0)return -1;
    unsigned state=atomic_load_explicit(&storage.state,memory_order_acquire);
    if(!state){storage.sector=sector;atomic_store_explicit(&storage.state,1,memory_order_release);
        if(board.handles[4])xTaskNotifyGive(board.handles[4]);return 0;}
    if(storage.sector!=sector)return -1;
    if(state==4)return -1;if(state!=3)return 0;
    memcpy(data,storage.data,512);atomic_store_explicit(&storage.state,0,memory_order_release);return 1;
}
int ss_key_report(struct ss_key_report *report)
{
    const struct ss_grant *g=ss_grant();
    if(!report||!g||!(g->devices&SS_DEVICE_USB_KEYBOARD)||ss_live()<0)return -1;
    unsigned ready=1;
    if(!atomic_compare_exchange_strong_explicit(&keyboard.ready,&ready,2,
        memory_order_acq_rel,memory_order_acquire))return 0;
    *report=keyboard.report;atomic_store_explicit(&keyboard.ready,0,memory_order_release);
    if(board.handles[4])xTaskNotifyGive(board.handles[4]);return 1;
}
#ifdef SUNSTONE_A733_OS_USB_CLASSES
static void usb_input(void *unused,const struct sunstone_hid_event *e)
{
    (void)unused;static uint64_t sequence;
    static struct {uint8_t address,instance,capabilities,keys[8];uint32_t buttons;} devices[8];
    unsigned slot;
    for(slot=0;slot<8;++slot)if(devices[slot].capabilities&&devices[slot].address==e->address&&devices[slot].instance==e->instance)break;
    if(e->type==SUNSTONE_HID_ATTACH){
        if(slot==8)for(slot=0;slot<8;++slot)if(!devices[slot].capabilities)break;
        if(slot==8){usb_fail();return;}
        devices[slot].address=e->address;devices[slot].instance=e->instance;devices[slot].capabilities=e->usage;
        memset(devices[slot].keys,0,8);devices[slot].buttons=0;
        unsigned connected=0;for(unsigned i=0;i<8;++i)connected|=devices[i].capabilities;
        os_usb_service_connected((connected&1?SS_DEVICE_USB_KEYBOARD:0)|(connected&2?SS_DEVICE_USB_MOUSE:0));return;
    }
    if(slot==8)return;
    if(e->type==SUNSTONE_HID_DETACH){
        devices[slot].capabilities=0;unsigned connected=0;for(unsigned i=0;i<8;++i)connected|=devices[i].capabilities;
        if(keyboard_pending.report.attachment==e->generation)keyboard_pending.dirty=0;
        if(keyboard_pending.attachment==e->generation)keyboard_pending.invalidate=1;
        keyboard_publish();
        os_usb_service_disconnected((connected&1?0:SS_DEVICE_USB_KEYBOARD)|(connected&2?0:SS_DEVICE_USB_MOUSE));return;
    }
    uint8_t *keys=devices[slot].keys;
    struct ss_input_event event={.attachment=e->generation,.sequence=++sequence,
        .usage=e->usage,.value=e->value,.x=e->x,.y=e->y,.wheel=e->wheel};
    if(e->type==SUNSTONE_HID_KEY){
        event.kind=SS_INPUT_KEY;
        if(e->usage>=0xe0&&e->usage<=0xe7){
            if(e->value)keys[0]|=1u<<(e->usage-0xe0);else keys[0]&=~(1u<<(e->usage-0xe0));
        }else if(e->usage<=255){
            unsigned i;for(i=2;i<8;++i)if(keys[i]==e->usage)break;
            if(!e->value&&i<8)keys[i]=0;
            if(e->value&&i==8)for(i=2;i<8;++i)if(!keys[i]){keys[i]=e->usage;break;}
        }
        event.modifiers=keys[0];
        keyboard_pending.report=(struct ss_key_report){.attachment=e->generation,.sequence=sequence,
            .address=e->address,.interface=e->instance,.protocol=1,.bytes=8};
        memcpy(keyboard_pending.report.data,keys,8);
        keyboard_pending.dirty=1;keyboard_publish();
    }else{
        event.kind=SS_INPUT_POINTER;
        if(e->type==SUNSTONE_HID_BUTTON&&e->usage>=1&&e->usage<=32){
            if(e->value)devices[slot].buttons|=1u<<(e->usage-1);else devices[slot].buttons&=~(1u<<(e->usage-1));
        }
        event.buttons=devices[slot].buttons;
    }
    unsigned device=event.kind==SS_INPUT_KEY?SS_DEVICE_USB_KEYBOARD:SS_DEVICE_USB_MOUSE;
    if((os_session_devices()&device)&&os_usb_service_input(&event)!=1)usb_fail();
}
#endif
static int serve(void)
{
#ifdef SUNSTONE_A733_OS_USB_CLASSES
    keyboard_publish();
#endif
    struct a733_usb2_native *n=&board.host;
    unsigned state=atomic_load_explicit(&storage.state,memory_order_acquire);
    if(state==1){
        if(!a733_usb2_read_sector(n,storage.sector,n->platform.config.lifetime_deadline))return 0;
        atomic_store_explicit(&storage.state,2,memory_order_release);state=2;
    }
    if(state==2){
        int r=a733_usb2_read_result(n,storage.data);if(r<0)return 0;
        if(r==1){memcpy(board.sector,storage.data,512);atomic_store_explicit(&board.result,1,memory_order_release);
            atomic_store_explicit(&storage.state,3,memory_order_release);os_application_usb_changed();}
    }
    if(os_session_devices()&SS_DEVICE_USB_KEYBOARD){
#ifdef SUNSTONE_A733_OS_USB_CLASSES
        return 1; /* Report-protocol HID callbacks publish both APIs. */
#endif
        if(!n->hid_mounted||n->hid.subclass!=1||n->hid.protocol!=1)return 0;
        if(n->hid.pending){
            struct a733_usb2_hid_report raw;int r=a733_usb2_hid_result(n,&raw);if(r<0)return 0;
            if(r==1){
                if(raw.generation!=n->core.generation||raw.generation!=n->platform.attachment.generation||raw.bytes!=8||
                   atomic_load_explicit(&keyboard.ready,memory_order_acquire))return 0;
                keyboard.report=(struct ss_key_report){.attachment=raw.generation,.sequence=raw.sequence,.address=raw.address,
                    .speed=raw.speed,.interface=raw.interface,.protocol=raw.protocol,.endpoint=raw.endpoint,
                    .subclass=1,.mode=0,.bytes=8};
                memcpy(keyboard.report.data,raw.data,8);atomic_store_explicit(&keyboard.ready,1,memory_order_release);
            }
        }
        if(!n->hid.pending&&!atomic_load_explicit(&keyboard.ready,memory_order_acquire)&&
           !a733_usb2_hid_receive(n,n->platform.config.lifetime_deadline))return 0;
    }
    return 1;
}
void os_storage_service(void){if(!serve())usb_fail();}
#ifdef SUNSTONE_BOARD_USB2_IRQ
static void host_wait(void)
{
#ifdef SUNSTONE_BOARD_HOST_DEADLINE
    /* The designated owner snapshots mutable host state before blocking.
     * Applications still use the mailbox; no EL0 caller enters the driver. */
    board_host_irq_schedule(a733_usb2_host_deadline(&board.host),
        a733_usb2_host_work_pending(&board.host));
#endif
    board_host_irq_wait();
}
#endif
void board_host_task(void *unused)
{
    if(os_quiesce_requested(&usb_quiesce))goto stopped;
    (void)unused;board.host.dual_leaf=!!(os_session_devices()&SS_DEVICE_USB_KEYBOARD);
#ifdef SUNSTONE_A733_OS_USB_CLASSES
    const struct os_usb_service_ops service_ops={.wake=usb_request_wake,
        .release=usb_release,.write=usb_write,.configure=usb_configure,
        .configured=usb_configured,.changed=usb_changed};
    os_usb_service_init(&service_ops,NULL);
    a733_usb2_generic_enable(&board.host);
    a733_usb2_set_input(&board.host,usb_input,NULL);
    struct sunstone_usb_transport *transport=a733_usb2_transport(&board.host);
    if(os_usb_serial_native_start(transport)!=1)usb_fail();
#endif
#ifdef SUNSTONE_BOARD_HOST_DEADLINE
    if(!board_host_irq_deadline_init())usb_fail();
#endif
    for(;;){
        if(os_quiesce_requested(&usb_quiesce))break;
        if(!board_live(NULL))break;
#ifdef SUNSTONE_BOARD_USB2_IRQ
        int ready=board_host_irq_before();if(ready<0)break;
        if(!ready){host_wait();continue;}
#endif
        enum a733_usb2_result r=a733_usb2_native_poll(&board.host);
        if(r==A733_USB2_RETAINED||r==A733_USB2_INVALID)break;
#ifdef SUNSTONE_BOARD_USB2_IRQ
        if(board_host_irq_after()<0)break;
#endif
#ifdef SUNSTONE_A733_OS_USB_CLASSES
        camera_poll();
#ifdef SUNSTONE_A733_OS_USB_JPEG
        decode_poll();
#endif
        os_usb_service_dispatch();
#endif
        os_application_host_observed(r==A733_USB2_READY);
        if(r==A733_USB2_READY&&!serve())break;
#ifdef SUNSTONE_BOARD_USB2_IRQ
        host_wait();
#else
        vTaskDelay(1);
#endif
    }
stopped:
#ifdef SUNSTONE_BOARD_USB2_IRQ
    board_host_irq_stop();
#endif
    /* Closing admission and masking the interrupt do not prove xHCI DMA
     * drained. The native host has no successful halt/release operation.
     * Keep its memory and leases, report that gap, and let the supervisor
     * collect the other owners' results before its bounded ending. */
#ifdef SUNSTONE_A733_OS_USB_CLASSES
    os_usb_service_retire();
#endif
    atomic_store_explicit(&storage.state,4,memory_order_release);
    if(os_quiesce_requested(&usb_quiesce))
        usb_quiesce_unproven(board.host.core_started);
    a733_usb2_native_lost(&board.host);a733_typec_revoke(&board.typec.provider);
    atomic_store_explicit(&board.result,2,memory_order_release);usb_fail();
    for(;;)vTaskSuspend(NULL);
}
