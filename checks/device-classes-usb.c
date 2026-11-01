/* SPDX-License-Identifier: GPL-2.0-only */
#include "sunstone_uvc_camera.h"
#include "usb-uvc-c270.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned released,starts,stops;
static int pending_start,pending_stop;
static uint64_t now;
static int clock_ok=1;
static int read_clock(void *context,uint64_t *value)
{(void)context;*value=now;return clock_ok;}
static void release_payload(void *context,void *lease)
{ (void)context;assert(lease);released++; }
static int begin(void *context,const struct sunstone_uvc_mode *mode,uint64_t deadline)
{ (void)context;assert(mode->width && deadline==1000);starts++;return !pending_start; }
static int end(void *context,uint64_t deadline)
{ (void)context;assert(deadline==1000);stops++;return !pending_stop; }
int main(void)
{
    struct sunstone_uvc_catalog catalog;
    assert(sunstone_uvc_descriptors(&catalog,c270_descriptors,sizeof(c270_descriptors))==0);
    struct sunstone_uvc stream;
    const struct sunstone_uvc_ops payload={.release=release_payload};
    sunstone_uvc_init(&stream,&payload,7,4096);
    struct sunstone_uvc_camera backend={0};
    const struct sunstone_uvc_camera_transport transport={begin,end,NULL,read_clock};
    struct sunstone_uvc_camera_transport no_clock=transport;
    no_clock.now=NULL;
    assert(sunstone_uvc_camera_init(&backend,&stream,&catalog,&no_clock)==-1);
    assert(sunstone_uvc_camera_init(&backend,&stream,&catalog,&transport)==1);
    assert(sunstone_uvc_camera_init(&backend,&stream,&catalog,&transport)==-1);
    struct sunstone_camera camera=backend.camera;
    struct sunstone_camera_mode mode,raw,unknown={1,1,1,1,1};
    unsigned modes=0,has_vga_15=0;
    while(camera.ops->enumerate(camera.context,modes,&raw)==1){
        assert(modes<4096 && raw.interval_num && raw.interval_den==10000000);
        if(raw.width==640 && raw.height==480 && raw.interval_num==666666)has_vga_15=1;
        modes++;
    }
    assert(modes>catalog.modes && has_vga_15);
    assert(camera.ops->enumerate(camera.context,modes,&raw)==0);
    assert(sunstone_uvc_camera_mode(&catalog.mode[19],&mode)==1 && mode.format==SUNSTONE_CAMERA_MJPEG);
    assert(camera.ops->start(camera.context,&unknown,1000)==-1 && !starts);
    pending_start=1;
    assert(camera.ops->start(camera.context,&mode,1000)==0 && starts==1);
    assert(camera.ops->start(camera.context,&mode,1001)==-1 && starts==1);
    assert(camera.ops->enumerate(camera.context,0,&raw)==1);
    assert(camera.ops->start(camera.context,&raw,1000)==-1 && starts==1);
    /* Stop during pending negotiation must ask the transport to retire it. */
    pending_stop=1;
    assert(camera.ops->stop(camera.context,1000)==0 && stops==1);
    assert(camera.ops->stop(camera.context,1001)==-1 && stops==1);
    assert(camera.ops->start(camera.context,&mode,1000)==-1 && starts==1);
    pending_stop=0;
    assert(camera.ops->stop(camera.context,1000)==1 && stops==2);
    pending_start=0;
    struct sunstone_camera_mode slow=mode;
    slow.interval_num=666666;
    assert(camera.ops->start(camera.context,&slow,1000)==1 && backend.active.interval==666666);
    assert(camera.ops->stop(camera.context,1000)==1);
    slow.interval_num=123456;
    assert(camera.ops->start(camera.context,&slow,1000)==-1);
    assert(camera.ops->start(camera.context,&mode,1000)==1);
    now=1000;
    unsigned old_starts=starts,old_stops=stops;
    assert(camera.ops->start(camera.context,&mode,1000)==-1 && starts==old_starts);
    assert(camera.ops->stop(camera.context,1000)==-1 && stops==old_stops);
    now=999;
    uint8_t first[]={2,0,0xff,0xd8},last[]={2,2,0xff,0xd9};
    stream.sequence=UINT64_C(0x100000000);
    assert(sunstone_uvc_payload(&stream,7,first,sizeof(first),first,0)==0);
    assert(sunstone_uvc_payload(&stream,7,last,sizeof(last),last,0)==0);
    struct sunstone_camera_frame frame;
    now=1000;
    assert(camera.ops->next(camera.context,&frame,1000)==-1 && !released);
    now=999;
    clock_ok=0;
    assert(camera.ops->next(camera.context,&frame,1000)==-1 && !released);
    clock_ok=1;
    assert(camera.ops->next(camera.context,&frame,1000)==1);
    const uint64_t old_token=frame.token;
    assert(frame.span_count==2 && frame.spans[0].data==first+2 && frame.spans[1].data==last+2);
    assert(frame.spans==stream.frame[0].slice && frame.bytes[0]==4 && !frame.plane[0]);
    assert(frame.sequence==UINT64_C(0x100000001) && !released);
    now=1000;
    assert(camera.ops->release(camera.context,old_token,1000)==-1 && !released);
    assert(backend.held[0].frame && backend.held[0].token==old_token);
    now=999;
    sunstone_uvc_disconnect(&stream);
    assert(!released && frame.spans[0].data[0]==0xff);
    assert(camera.ops->next(camera.context,&frame,1000)==-1);
    assert(camera.ops->release(camera.context,old_token,1000)==1 && released==2);
    assert(camera.ops->release(camera.context,old_token,1000)==-1 && released==2);
    assert(camera.ops->stop(camera.context,1000)==1);
    sunstone_uvc_init(&stream,&payload,8,4096);
    assert(camera.ops->start(camera.context,&mode,1000)==1);
    assert(sunstone_uvc_payload(&stream,8,first,sizeof(first),first,0)==0);
    assert(sunstone_uvc_payload(&stream,8,last,sizeof(last),last,0)==0);
    assert(camera.ops->next(camera.context,&frame,1000)==1 && frame.token>old_token);
    assert(camera.ops->release(camera.context,old_token,1000)==-1 && released==2);
    pending_stop=1;
    assert(camera.ops->stop(camera.context,1000)==0 && released==2);
    pending_stop=0;
    assert(camera.ops->stop(camera.context,1000)==1 && released==2);
    assert(camera.ops->release(camera.context,frame.token,1000)==1 && released==4);
    assert(camera.ops->start(camera.context,&mode,0)==-1);
    struct sunstone_uvc_catalog continuous={.modes=1,
        .mode={{SUNSTONE_UVC_YUYV,1,1,1,16,16,100000,512}},
        .intervals={{.minimum=100000,.maximum=200000,.step=25000}}};
    struct sunstone_uvc_camera range={0};
    assert(sunstone_uvc_camera_init(&range,&stream,&continuous,&transport)==1);
    assert(range.camera.ops->enumerate(&range,4,&raw)==1 && raw.interval_num==200000);
    assert(range.camera.ops->enumerate(&range,5,&raw)==0);
    assert(range.camera.ops->start(&range,&raw,1000)==1 && range.active.interval==200000);
    assert(range.camera.ops->stop(&range,1000)==1);
    raw.interval_num=190000;
    assert(range.camera.ops->start(&range,&raw,1000)==-1);
    puts("PASS: shared camera modes, absolute deadlines, asynchronous lifecycle, original spans, detach and stale tickets");
    return 0;
}
