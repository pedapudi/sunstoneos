/* SPDX-License-Identifier: GPL-2.0-only */
#include "sunstone_uvc_camera.h"
#include <string.h>

int sunstone_uvc_camera_mode(const struct sunstone_uvc_mode *m,struct sunstone_camera_mode *out)
{
    if(!m || !out || !m->width || !m->height || !m->interval ||
       (m->format!=SUNSTONE_UVC_YUYV && m->format!=SUNSTONE_UVC_MJPEG))return -1;
    *out=(struct sunstone_camera_mode){m->width,m->height,
        m->format==SUNSTONE_UVC_YUYV?SUNSTONE_CAMERA_YUYV:SUNSTONE_CAMERA_MJPEG,
        m->interval,10000000};
    return 1;
}
int sunstone_uvc_camera_view(const struct sunstone_uvc_frame *f,
    const struct sunstone_uvc_mode *m,uint64_t token,struct sunstone_camera_frame *out)
{
    struct sunstone_camera_mode mode;
    if(!f || !out || !token || f->state!=2 || !f->slices ||
       f->slices>SUNSTONE_UVC_SLICES || sunstone_uvc_camera_mode(m,&mode)!=1)return -1;
    *out=(struct sunstone_camera_frame){.bytes={f->bytes,0},
        .stride={mode.format==SUNSTONE_CAMERA_YUYV?mode.width*2u:0,0},
        .spans=f->slice,.span_count=f->slices,.sequence=f->sequence,.token=token};
    return 1;
}
static int enumerate(void *context,unsigned index,struct sunstone_camera_mode *mode)
{
    const struct sunstone_uvc_camera *s=context;
    if(!mode || s->fault)return -1;
    for(unsigned i=0;i<s->catalog->modes;i++){
        uint64_t count=s->catalog->intervals[i].count;
        if(!count)count=s->catalog->intervals[i].step?
            1u+(uint64_t)(s->catalog->intervals[i].maximum-s->catalog->intervals[i].minimum)/s->catalog->intervals[i].step:1u;
        if(index>=count){index-=(unsigned)count;continue;}
        struct sunstone_uvc_mode selected=s->catalog->mode[i];
        if(s->catalog->intervals[i].count)selected.interval=s->catalog->intervals[i].discrete[index];
        else if(s->catalog->intervals[i].step)selected.interval=s->catalog->intervals[i].minimum+index*s->catalog->intervals[i].step;
        return sunstone_uvc_camera_mode(&selected,mode);
    }
    return 0;
}
static int matches(const struct sunstone_uvc_mode *m,const struct sunstone_camera_mode *wanted)
{
    struct sunstone_camera_mode mode;
    return wanted && wanted->interval_num && wanted->interval_den &&
        sunstone_uvc_camera_mode(m,&mode)==1 && mode.width==wanted->width &&
        mode.height==wanted->height && mode.format==wanted->format &&
        (uint64_t)mode.interval_num*wanted->interval_den==
        (uint64_t)wanted->interval_num*mode.interval_den;
}
static int select_interval(const struct sunstone_uvc_catalog *catalog,unsigned index,
    const struct sunstone_camera_mode *wanted,struct sunstone_uvc_mode *selected)
{
    if(!wanted || !wanted->interval_num || !wanted->interval_den)return 0;
    uint64_t ticks=(uint64_t)wanted->interval_num*10000000u;
    if(ticks%wanted->interval_den || ticks/wanted->interval_den>UINT32_MAX)return 0;
    *selected=catalog->mode[index];selected->interval=(uint32_t)(ticks/wanted->interval_den);
    if(!matches(selected,wanted))return 0;
    if(catalog->intervals[index].count){
        for(unsigned j=0;j<catalog->intervals[index].count;j++)
            if(catalog->intervals[index].discrete[j]==selected->interval)return 1;
        return 0;
    }
    if(catalog->intervals[index].step)return selected->interval>=catalog->intervals[index].minimum &&
        selected->interval<=catalog->intervals[index].maximum &&
        !((selected->interval-catalog->intervals[index].minimum)%catalog->intervals[index].step);
    return selected->interval==catalog->mode[index].interval;
}
static int start(void *context,const struct sunstone_camera_mode *mode,uint64_t deadline)
{
    struct sunstone_uvc_camera *s=context;
    if(s->fault || s->stopping || !deadline || !mode)return -1;
    if(s->started)return matches(&s->active,mode)?1:-1;
    if(s->starting && !matches(&s->active,mode))return -1;
    /* Mode search is setup-only. Frame acquire/release inspect at most the
     * three fixed lease slots and never search modes or USB descriptors. */
    for(unsigned i=0;i<SUNSTONE_UVC_FRAMES;i++)if(s->held[i].frame)return -1;
    for(unsigned i=0;i<s->catalog->modes;i++){
        struct sunstone_uvc_mode selected;
        if(!select_interval(s->catalog,i,mode,&selected))continue;
        if(!s->starting){s->active=selected;s->starting=1;}
        int r=s->transport.start(s->transport.context,&s->active,deadline);
        if(r<0 || r>1){s->fault=1;return -1;}
        if(r==1){s->starting=0;s->started=1;}
        return r;
    }
    return -1;
}
static int next(void *context,struct sunstone_camera_frame *view,uint64_t deadline)
{
    struct sunstone_uvc_camera *s=context;
    if(s->fault || s->stopping || !view || !deadline || !s->started || !s->stream->connected)return -1;
    if(s->token==UINT64_MAX){s->fault=1;return -1;}
    unsigned slot;
    for(slot=0;slot<SUNSTONE_UVC_FRAMES;slot++)if(!s->held[slot].frame)break;
    if(slot==SUNSTONE_UVC_FRAMES)return 0;
    const struct sunstone_uvc_frame *frame=sunstone_uvc_acquire(s->stream);
    if(!frame)return 0;
    s->held[slot].frame=frame;
    s->held[slot].generation=frame->generation;s->held[slot].sequence=frame->sequence;
    s->held[slot].token=++s->token;
    if(sunstone_uvc_camera_view(frame,&s->active,s->token,view)!=1){s->fault=1;return -1;}
    return 1;
}
static int release(void *context,uint64_t token,uint64_t deadline)
{
    struct sunstone_uvc_camera *s=context;
    if(!token || !deadline)return -1;
    /* Release remains available after detach/stop/fault. A stale ticket never
     * releases a later frame at the same address. */
    for(unsigned i=0;i<SUNSTONE_UVC_FRAMES;i++)if(s->held[i].frame && s->held[i].token==token){
        if(sunstone_uvc_release_ticket(s->stream,s->held[i].frame,
            s->held[i].generation,s->held[i].sequence)!=0){s->fault=1;return -1;}
        s->held[i].frame=NULL;return 1;
    }
    return -1;
}
static int stop(void *context,uint64_t deadline)
{
    struct sunstone_uvc_camera *s=context;
    if(s->fault || !deadline)return -1;
    if(!s->started && !s->starting)return 1;
    s->stopping=1;
    int r=s->transport.stop(s->transport.context,deadline);
    if(r<0 || r>1){s->fault=1;return -1;}
    if(r==1){s->started=0;s->starting=0;s->stopping=0;}
    return r;
}
static const struct sunstone_camera_ops operations={enumerate,start,next,release,stop};
int sunstone_uvc_camera_init(struct sunstone_uvc_camera *s,struct sunstone_uvc *stream,
    const struct sunstone_uvc_catalog *catalog,const struct sunstone_uvc_camera_transport *transport)
{
    if(!s || s->camera.ops || !stream || !catalog || !transport || !transport->start || !transport->stop ||
       !catalog->modes || catalog->modes>SUNSTONE_UVC_MODES)return -1;
    for(unsigned i=0;i<catalog->modes;i++){
        struct sunstone_camera_mode mode;
        if(sunstone_uvc_camera_mode(&catalog->mode[i],&mode)!=1 ||
           catalog->intervals[i].count>SUNSTONE_UVC_INTERVALS)return -1;
        if(catalog->intervals[i].count){
            if(catalog->intervals[i].step)return -1;
            for(unsigned j=0;j<catalog->intervals[i].count;j++)if(!catalog->intervals[i].discrete[j])return -1;
        }else if(catalog->intervals[i].step && (!catalog->intervals[i].minimum ||
                 catalog->intervals[i].maximum<catalog->intervals[i].minimum))return -1;
    }
    memset(s,0,sizeof(*s));s->camera=(struct sunstone_camera){&operations,s};
    s->stream=stream;s->catalog=catalog;s->transport=*transport;return 1;
}
