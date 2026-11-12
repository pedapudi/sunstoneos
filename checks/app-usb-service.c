#include "sunstone_usb_service.h"
#include <assert.h>
static struct ss_grant grant;
static int live=1,writes,wakes,configured,configurations;
static unsigned serial_releases,camera_releases;
const struct ss_grant *ss_grant(void){return &grant;}
int ss_live(void){return live;}
static void wake(void *c){(void)c;++wakes;}
static void release(void *c,unsigned d,uint64_t token)
{
    (void)c;assert(token);
    if(d==SS_DEVICE_USB_SERIAL)++serial_releases;
    else{assert(d==SS_DEVICE_USB_CAMERA);++camera_releases;}
}
static int write(void *c,uint64_t sequence,const void *p,size_t bytes)
{(void)c;assert(sequence>=9&&p&&bytes==3);++writes;return 1;}
static int configure(void *c,const struct ss_usb_serial_line *line)
{(void)c;assert(line->baud==1000000&&line->bits==8);++configurations;return 1;}
static int config_result(void *c){(void)c;return configured;}
int main(void)
{
    const struct os_usb_service_ops ops={.wake=wake,.release=release,.write=write,
        .configure=configure,.configured=config_result};
    os_usb_service_init(&ops,0);
    char bytes[]="abc";struct ss_usb_serial_buffer value={7,1,(void *)bytes,3},out;
    assert(ss_usb_serial_acquire(&out)==-1);
    grant.devices=SS_DEVICE_USB_SERIAL;assert(ss_usb_serial_acquire(&out)==0);
    os_usb_service_connected(grant.devices);
    assert(os_usb_service_serial(&value)==1);assert(os_usb_service_serial(&value)==-1);
    assert(ss_usb_serial_acquire(&out)==1&&out.data==(void *)bytes);
    assert(ss_usb_serial_acquire(&out)==0);assert(ss_usb_serial_release(8)==-1);
    assert(ss_usb_serial_write(9,bytes,3)==1&&wakes==1);size_t actual=99;
    assert(ss_usb_serial_write_result(9,&actual)==0);os_usb_service_dispatch();assert(writes==1);
    os_usb_service_disconnected(grant.devices);assert(ss_usb_serial_acquire(&out)==-1);
    live=-1;assert(ss_usb_serial_write_result(9,&actual)==0);
    assert(ss_usb_serial_release(7)==1);os_usb_service_dispatch();assert(serial_releases==1);
    os_usb_service_write_complete(9,-1,0);
    assert(ss_usb_serial_write_result(9,&actual)==-1&&actual==0);
    assert(ss_usb_serial_release(7)==-1);live=1;

    /* Queued unread data retires on detach and cannot cross reconnect. */
    os_usb_service_connected(grant.devices);value.token=8;
    assert(os_usb_service_serial(&value)==1);os_usb_service_disconnected(grant.devices);
    os_usb_service_connected(grant.devices);assert(ss_usb_serial_acquire(&out)==0);
    os_usb_service_dispatch();assert(serial_releases==2);
    assert(os_usb_service_serial(&value)==-1);
    const struct ss_usb_serial_line line={1000000,8,0,0,0};
    assert(ss_usb_serial_configure(1,&line)==1);os_usb_service_dispatch();
    assert(ss_usb_serial_config_result(1)==0);configured=1;os_usb_service_dispatch();
    assert(ss_usb_serial_config_result(1)==1);

    /* An ungranted input class is skipped, and old epochs are discarded. */
    grant.devices|=SS_DEVICE_USB_KEYBOARD;
    os_usb_service_connected(SS_DEVICE_USB_KEYBOARD|SS_DEVICE_USB_MOUSE);
    struct ss_input_event input={.kind=SS_INPUT_POINTER,.x=2},event;
    assert(os_usb_service_input(&input)==1);assert(ss_input_event(&event)==0);
    input.kind=SS_INPUT_KEY;input.usage=4;assert(os_usb_service_input(&input)==1);
    os_usb_service_disconnected(SS_DEVICE_USB_KEYBOARD);os_usb_service_connected(SS_DEVICE_USB_KEYBOARD);
    assert(ss_input_event(&event)==0);assert(os_usb_service_input(&input)==1);
    assert(ss_input_event(&event)==1&&event.usage==4);
    os_usb_service_disconnected(SS_DEVICE_USB_MOUSE);assert(ss_input_event(&event)==0);

    /* A pinned frame rejects release until the OS finishes its gather. */
    grant.devices|=SS_DEVICE_USB_CAMERA;os_usb_service_connected(SS_DEVICE_USB_CAMERA);
    const struct ss_usb_frame_slice slice={(void *)bytes,3};
    const struct ss_usb_camera_frame frame={.token=1,.sequence=2,.slices=&slice,.slice_count=1,.bytes=3};
    struct ss_usb_camera_frame camera;
    assert(os_usb_service_frame(&frame)==1);assert(ss_usb_camera_acquire(&camera)==1);
    assert(camera.slices==&slice&&camera.slices[0].data==(void *)bytes);
    assert(os_usb_service_camera_pin(1,&camera)==1);assert(ss_usb_camera_release(1)==-1);
    os_usb_service_camera_unpin(1);assert(ss_usb_camera_release(1)==1);
    os_usb_service_dispatch();assert(camera_releases==1);

    /* Application exit retains receive memory while zero-copy TX borrows it. */
    value.token=9;assert(os_usb_service_serial(&value)==1);assert(ss_usb_serial_acquire(&out)==1);
    assert(ss_usb_serial_write(10,out.data,out.bytes)==1);os_usb_service_dispatch();
    assert(ss_usb_serial_configure(2,&line)==1);
    os_usb_service_retire();os_usb_service_dispatch();assert(!os_usb_service_idle()&&serial_releases==2);
    assert(configurations==1&&ss_usb_serial_config_result(2)==-1);
    /* Controller stop proof lets the host retire an aborted transmit. */
    os_usb_service_stopped();os_usb_service_dispatch();
    assert(os_usb_service_idle()&&serial_releases==3);
    assert(ss_usb_serial_write_result(10,&actual)==-1&&actual==0);return 0;
}
