/* SPDX-License-Identifier: GPL-2.0-only */
#include "a733_camera_application.h"
#include <string.h>
static int fault(struct a733_camera_application *a){a->failed=1;return -1;}
static int enumerate(void *context,unsigned index,struct sunstone_camera_mode *out)
{
    struct a733_camera_application *a=context;
    if(!a || !a->native || a->failed || !out)return -1;
    if(index)return 0;
    const struct camera_mode *m=&a->native->capture.sensor.mode;
    uint32_t ticks=(uint32_t)m->htotal*m->vtotal;
    if(!ticks || !m->pixel_hz)return -1;
    *out=(struct sunstone_camera_mode){m->width,m->height,SUNSTONE_CAMERA_NV16,ticks,m->pixel_hz};return 1;
}
static int deadline(const struct a733_camera_application *a,uint64_t d)
{return d && d==a->native->boot.deadline_us;}
static int start(void *context,const struct sunstone_camera_mode *requested,uint64_t d)
{
    struct a733_camera_application *a=context;struct sunstone_camera_mode mode;
    if(!a || a->failed || !requested || !deadline(a,d) || a->started || a->stopping ||
       enumerate(a,0,&mode)!=1 || requested->width!=mode.width || requested->height!=mode.height ||
       requested->format!=mode.format || !requested->interval_num || !requested->interval_den ||
       (uint64_t)requested->interval_num*mode.interval_den!=(uint64_t)mode.interval_num*requested->interval_den)return -1;
    a->starting=1;int r=a733_camera_native_poll(a->native);
    if(r<0)return fault(a);
    if(r==1){a->started=1;a->starting=0;}return r;
}
static int next(void *context,struct sunstone_camera_frame *frame,uint64_t d)
{
    struct a733_camera_application *a=context;
    if(!a || a->failed || !a->started || a->stopping || a->token || !frame || !deadline(a,d))return -1;
    int r=a733_camera_native_poll(a->native);if(r<0)return fault(a);if(!r)return 0;
    const uint8_t *y,*uv;unsigned slot;uint32_t sequence;
    r=a733_camera_capture_take(&a->native->capture,&slot,&sequence,&y,&uv);
    if(r<0)return fault(a);if(!r)return 0;
    a->slot=slot;
    if(a->next_token==UINT64_MAX)return fault(a);
    a->token=++a->next_token;
    *frame=(struct sunstone_camera_frame){.plane={y,uv},
        .bytes={A733_CAMERA_NATIVE_PLANE_BYTES,A733_CAMERA_NATIVE_PLANE_BYTES},
        .stride={A733_CAMERA_WIDTH,A733_CAMERA_WIDTH},.sequence=sequence,.token=a->token};return 1;
}
static int release(void *context,uint64_t token,uint64_t d)
{
    struct a733_camera_application *a=context;
    if(!a || a->failed || !token || token!=a->token || !deadline(a,d))return -1;
    if(a733_camera_native_poll(a->native)!=1 || a733_camera_capture_give(&a->native->capture,a->slot)!=1)return fault(a);
    a->token=0;return 1;
}
static int stop(void *context,uint64_t d)
{
    struct a733_camera_application *a=context;
    if(!a || a->failed || !a->started || a->token || !deadline(a,d))return -1;
    if(!a->stopping){
        if(a733_camera_native_stream_stop(a->native)!=1)return fault(a);
        a->stopping=1;
    }
    int r=a733_camera_native_poll(a->native);
    if(r<0)return fault(a);
    if(a->native->capture.state==A733_CAPTURE_READY){a->started=0;return 1;}
    return 0;
}
static const struct sunstone_camera_ops operations={enumerate,start,next,release,stop};
int a733_camera_application_init(struct a733_camera_application *a,struct a733_camera_native *native,
    struct sunstone_camera *camera)
{
    if(!a || !native || !camera || a->native || native->self!=native || !native->stream ||
       native->state==A733_CAMERA_NATIVE_FAULT)return -1;
    *a=(struct a733_camera_application){.native=native};
    *camera=(struct sunstone_camera){&operations,a};return 1;
}
