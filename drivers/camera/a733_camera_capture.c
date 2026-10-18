/* SPDX-License-Identifier: GPL-2.0-only */
#include "a733_camera_capture.h"
#include <stddef.h>

static int fail(struct a733_camera_capture *s){s->state=A733_CAPTURE_FAULT;return -1;}
static int live(const struct a733_camera_capture *s){return s->token && s->state!=A733_CAPTURE_FAULT;}
static int clock_read(void *p,uint64_t *n){
    struct a733_camera_capture *s=p;
    return s->state==A733_CAPTURE_FAULT?-1:s->config.ops.now_us(s->config.context,n);
}
static int tick(struct a733_camera_capture *s){
    uint64_t n;
    if(clock_read(s,&n)!=1 || n<s->now || n>=s->deadline)return fail(s);
    s->now=n;return s->state==A733_CAPTURE_FAULT?-1:1;
}
#define BEGIN struct a733_camera_capture *s=p; if(!live(s))return -1
#define CALL(group,name,...) s->config.group.name(s->config.context,__VA_ARGS__)
#define SIMPLE(fn,group,name) \
static int fn(void *p,uint64_t t,uint64_t d){BEGIN;return CALL(group,name,t,d);}
static int analog_admit(void *p,uint64_t d,struct a733_camera_admission *a){BEGIN;return CALL(analog,admit,d,a);}
static int analog_read(void *p,uint64_t t,uint16_t o,uint32_t *v,uint64_t d){BEGIN;return CALL(analog,read,t,o,v,d);}
static int analog_write(void *p,uint64_t t,uint16_t o,uint32_t v,uint64_t d){BEGIN;return CALL(analog,write,t,o,v,d);}
static int mmu_admit(void *p,const struct a733_iommu_tables *x,uint64_t d,struct a733_iommu_admission *a){
    BEGIN;return CALL(iommu,admit,x,d,a);
}
static int mmu_read(void *p,uint64_t t,uint32_t o,uint32_t *v,uint64_t d){BEGIN;return CALL(iommu,read,t,o,v,d);}
static int mmu_write(void *p,uint64_t t,uint32_t o,uint32_t v,uint64_t d){BEGIN;return CALL(iommu,write,t,o,v,d);}
static int mmu_publish(void *p,uint64_t t,const void *x,size_t n,uint64_t d){BEGIN;return CALL(iommu,publish,t,x,n,d);}
static int sensor_acquire(void *p,uint64_t d,uint64_t *t){BEGIN;return CALL(sensor,acquire,d,t);}
static int sensor_line(void *p,uint64_t t,enum ov5640_line l,int a,uint64_t d){BEGIN;return CALL(sensor,line,t,l,a,d);}
static int sensor_read(void *p,uint64_t t,uint8_t a,uint16_t r,uint8_t *v,uint64_t d){BEGIN;return CALL(sensor,read,t,a,r,v,d);}
static int sensor_write(void *p,uint64_t t,uint8_t a,uint16_t r,uint8_t v,uint64_t d){BEGIN;return CALL(sensor,write,t,a,r,v,d);}
static int sensor_receiver(void *p,uint64_t t,uint64_t rt,const struct ov5640_mode *m,uint64_t d){
    BEGIN;
    if(!s->packets_granted || rt!=s->receiver.token || s->receiver.state!=A733_CSI_ARMED ||
       !s->receiver.image_mode || s->route.state!=A733_ROUTE_ARMED ||
       s->dma.state!=A733_CAM_DMA_START || s->iommu.state!=A733_IOMMU_ACTIVE)return -1;
    return CALL(sensor,receiver,t,rt,m,d);
}
SIMPLE(sensor_release,sensor,release)
static int route_admit(void *p,const struct a733_route_config *c,uint64_t d,uint64_t *t){
    BEGIN;
    if(s->analog.state!=A733_CAMERA_PLATFORM_ACTIVE || s->iommu.state!=A733_IOMMU_ACTIVE ||
       s->sensor.state!=OV5640_READY)return -1;
    return CALL(route,admit,c,d,t);
}
static int route_publish(void *p,uint64_t t,const uint8_t x[1024],uint64_t d){BEGIN;return CALL(route,publish,t,x,d);}
static int route_read(void *p,uint64_t t,enum a733_route_region r,uint16_t o,uint32_t *v,uint64_t d){BEGIN;return CALL(route,read,t,r,o,v,d);}
static int route_write(void *p,uint64_t t,enum a733_route_region r,uint16_t o,uint32_t v,uint64_t d){BEGIN;return CALL(route,write,t,r,o,v,d);}
SIMPLE(route_drain,route,drain)
SIMPLE(route_release,route,release)
static int csi_admit(void *p,const struct a733_csi_config *c,uint64_t d,uint64_t *t){
    BEGIN;
    if(!s->local_granted || s->analog.state!=A733_CAMERA_PLATFORM_ACTIVE ||
       s->iommu.state!=A733_IOMMU_ACTIVE || s->sensor.state!=OV5640_READY ||
       s->route.state!=A733_ROUTE_ARMED || s->dma.state!=A733_CAM_DMA_ARMED)return -1;
    return CALL(csi,admit,c,d,t);
}
static int csi_read(void *p,uint64_t t,enum a733_csi_region r,uint16_t o,uint32_t *v,uint64_t d){BEGIN;return CALL(csi,read,t,r,o,v,d);}
static int csi_write(void *p,uint64_t t,enum a733_csi_region r,uint16_t o,uint32_t v,uint64_t d){BEGIN;return CALL(csi,write,t,r,o,v,d);}
SIMPLE(csi_drain,csi,drain)
SIMPLE(csi_release,csi,release)
static int dma_admit(void *p,const struct a733_camera_dma_config *c,uint64_t d,uint64_t *t){
    BEGIN;
    if(s->iommu.state!=A733_IOMMU_ACTIVE || s->route.state!=A733_ROUTE_ARMED ||
       s->sensor.state!=OV5640_READY)return -1;
    return CALL(dma,admit,c,d,t);
}
static int dma_read(void *p,uint64_t t,uint16_t o,uint32_t *v,uint64_t d){BEGIN;return CALL(dma,read,t,o,v,d);}
static int dma_write(void *p,uint64_t t,uint16_t o,uint32_t v,uint64_t d){BEGIN;return CALL(dma,write,t,o,v,d);}
static int dma_start(void *p,uint64_t t,uint64_t d){
    BEGIN;(void)d;
    return t==s->dma.token && s->packets_granted && s->sensor.state==OV5640_STREAMING?1:-1;
}
static int stopped(void *p,uint64_t t,uint64_t d){
    BEGIN;(void)t;(void)d;
    if(s->retained_readout)return s->sampled && s->fenced && s->scanned &&
        s->sensor.state==OV5640_READY && s->receiver.state==A733_CSI_ARMED &&
        s->receiver.image_mode && s->route.state==A733_ROUTE_ARMED &&
        s->iommu.state==A733_IOMMU_ACTIVE && s->iommu.fault.complete &&
        (s->dma.events&1)?1:-1;
    return s->quiet && s->sampled && s->fenced && s->sensor.state==OV5640_READY &&
        (s->dma.events&1)?1:-1;
}
static int dma_drained(void *p,uint64_t t,uint64_t d){
    BEGIN;(void)d;
    return t==s->dma.token && s->drained && s->scanned &&
        s->iommu.state==A733_IOMMU_ACTIVE && s->route.state==A733_ROUTE_CLOSED &&
        s->receiver.state==A733_CSI_CLOSED && s->sensor.state==OV5640_CLOSED?1:-1;
}
static int dma_sync(void *p,uint64_t t,uint64_t d){
    BEGIN;
    if(s->retained_readout) {
        if(t!=s->dma.token || stopped(p,t,d)!=1)return -1;
    } else if(dma_drained(p,t,d)!=1)return -1;
    return CALL(dma,sync_cpu,t,d);
}
SIMPLE(dma_release,dma,release)
#undef SIMPLE
#undef CALL
#undef BEGIN

static int overlap(const void *a,size_t an,const void *b,size_t bn){
    uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
    return an>UINTPTR_MAX-x || bn>UINTPTR_MAX-y || (x<y+bn && y<x+an);
}
static int callbacks(const struct a733_camera_capture_config *c){
    return c->ops.now_us && c->ops.acquire && c->ops.handoff && c->ops.packets_quiet &&
        c->analog.admit && c->analog.read && c->analog.write && c->iommu.admit &&
        c->iommu.read && c->iommu.write && c->iommu.publish && c->sensor.acquire &&
        c->sensor.line && c->sensor.read && c->sensor.write && c->sensor.receiver &&
        c->sensor.release && c->route.admit && c->route.read && c->route.write &&
        c->route.publish && c->route.drain && c->route.release && c->csi.admit &&
        c->csi.read && c->csi.write && c->csi.drain && c->csi.release &&
        c->dma.admit && c->dma.read && c->dma.write && c->dma.drain &&
        c->dma.sync_cpu && c->dma.release;
}
static int initialize(struct a733_camera_capture *s,const struct a733_camera_capture_config *c,unsigned supervised){
    if(!s || !c || s->initialized || s->state!=A733_CAPTURE_NEW ||
       overlap(s,sizeof(*s),c,sizeof(*c)) || (c->context && overlap(s,sizeof(*s),c->context,1)) ||
       !callbacks(c) || !c->frame_id || !c->table_words || ((uintptr_t)c->table_words&16383) ||
       c->receiver.port>1 || c->receiver.parser!=c->receiver.port ||
       !c->receiver.settle || !c->receiver.deskew || c->receiver.deskew>31 ||
       c->receiver.top_hz!=324000000 || (c->color_bars!=0 && c->color_bars!=1) ||
       overlap(s,sizeof(*s),c->table_words,A733_IOMMU_TABLE_BYTES) ||
       overlap(c,sizeof(*c),c->table_words,A733_IOMMU_TABLE_BYTES))return -1;
    struct a733_iommu_span spans[3];
    for(unsigned i=0;i<3;i++){
        const struct a733_capture_span *m=&c->memory[i];
        if(!m->cpu || ((uintptr_t)m->cpu&4095) || !m->iova ||
           m->iova>=UINT64_C(0x100000000) || m->bytes>UINT64_C(0x100000000)-m->iova ||
           m->bytes<(i?307200u:4096u) || (m->bytes&4095) || m->bytes>SIZE_MAX ||
           overlap(s,sizeof(*s),m->cpu,(size_t)m->bytes) ||
           overlap(c,sizeof(*c),m->cpu,(size_t)m->bytes) ||
           overlap(c->table_words,A733_IOMMU_TABLE_BYTES,m->cpu,(size_t)m->bytes))return -1;
        for(unsigned j=0;j<i;j++)if(overlap(c->memory[j].cpu,(size_t)c->memory[j].bytes,m->cpu,(size_t)m->bytes))return -1;
        spans[i]=(struct a733_iommu_span){m->iova,m->physical,m->bytes,i?A733_IOMMU_WRITE:A733_IOMMU_READ};
    }
    s->config=*c;s->initialized=1;
    if(a733_iommu_build(&s->tables,c->table_words,c->table_physical,spans,3)!=1)return fail(s);
    struct a733_camera_platform_ops a={clock_read,analog_admit,analog_read,analog_write};
    struct a733_iommu_ops i={clock_read,mmu_admit,mmu_read,mmu_write,mmu_publish};
    struct ov5640_ops o={clock_read,sensor_acquire,sensor_line,sensor_read,sensor_write,sensor_receiver,sensor_release};
    struct a733_route_ops r={clock_read,route_admit,route_publish,route_read,route_write,stopped,route_drain,route_release};
    struct a733_csi_ops v={clock_read,csi_admit,csi_read,csi_write,stopped,csi_drain,csi_release};
    struct a733_camera_dma_ops d={clock_read,dma_admit,dma_read,dma_write,dma_start,stopped,dma_drained,dma_sync,dma_release};
    struct a733_route_config rc={c->memory[0].iova,c->memory[0].bytes,c->receiver.parser};
    struct a733_camera_dma_config dc={
        {c->memory[1].iova,c->memory[1].bytes},{c->memory[2].iova,c->memory[2].bytes},c->frame_id,0};
    if((supervised?a733_camera_platform_supervised_init(&s->analog,&a,s):a733_camera_platform_init(&s->analog,&a,s))!=1 || (supervised?a733_iommu_init_camera_supervised(&s->iommu,&i,s,&s->tables):a733_iommu_init(&s->iommu,&i,s,&s->tables))!=1 ||
       ov5640_init(&s->sensor,&o,s,c->color_bars)!=1 || a733_camera_route_init(&s->route,&r,s,&rc)!=1 ||
       a733_csi_init(&s->receiver,&v,s,&c->receiver)!=1 || a733_camera_dma_init(&s->dma,&d,s,&dc)!=1)return fail(s);
    return 1;
}
int a733_camera_capture_init(struct a733_camera_capture *s,const struct a733_camera_capture_config *c)
{return initialize(s,c,0);}
int a733_camera_capture_supervised_init(struct a733_camera_capture *s,const struct a733_camera_capture_config *c)
{return initialize(s,c,1);}
int a733_camera_capture_open(struct a733_camera_capture *s,uint64_t d){
    if(!s || !s->initialized || s->entered || s->state!=A733_CAPTURE_NEW || !d)return -1;
    s->deadline=d;s->state=A733_CAPTURE_ACQUIRE;return 1;
}
int a733_camera_capture_open_retained(struct a733_camera_capture *s,uint64_t d){
    if(a733_camera_capture_open(s,d)!=1)return -1;
    s->retained_readout=1;return 1;
}
static void next(struct a733_camera_capture *s,enum a733_capture_state n){
    if(s->state!=A733_CAPTURE_FAULT){s->state=n;s->started=0;}
}
#define RUN(begin,poll,target) do { \
    if(!s->started){if((begin)!=1){r=-1;break;}s->started=1;} \
    r=(poll);if(r==1)next(s,target); \
} while(0)
int a733_camera_capture_poll(struct a733_camera_capture *s){
    if(!s || !s->initialized || s->state==A733_CAPTURE_NEW || s->state==A733_CAPTURE_FAULT)return -1;
    if(s->entered)return fail(s);
    if(s->state==A733_CAPTURE_READY || s->state==A733_CAPTURE_READABLE)return 1;
    s->entered=1;int r=0;uint64_t token;uint32_t v;
    if(tick(s)!=1)goto out;
    switch(s->state){
    case A733_CAPTURE_ACQUIRE:
        token=s->token;r=s->config.ops.acquire(s->config.context,&s->config,s->deadline,&token);
        if(s->token && token!=s->token){r=-1;break;}s->token=token;
        if(r==1){if(!token){r=-1;break;}next(s,A733_CAPTURE_ANALOG);}break;
    case A733_CAPTURE_ANALOG:
        RUN(a733_camera_platform_open(&s->analog,s->deadline),a733_camera_platform_poll(&s->analog),A733_CAPTURE_IOMMU);break;
    case A733_CAPTURE_IOMMU:
        RUN(a733_iommu_open(&s->iommu,s->deadline),a733_iommu_poll(&s->iommu),A733_CAPTURE_SENSOR_OPEN);break;
    case A733_CAPTURE_SENSOR_OPEN:
        RUN(ov5640_open(&s->sensor,s->deadline),ov5640_poll(&s->sensor),A733_CAPTURE_ROUTE_OPEN);break;
    case A733_CAPTURE_ROUTE_OPEN:
        RUN(a733_camera_route_open(&s->route,s->deadline),a733_camera_route_poll(&s->route),A733_CAPTURE_DMA_OPEN);break;
    case A733_CAPTURE_DMA_OPEN:
        RUN(a733_camera_dma_open(&s->dma,s->deadline),a733_camera_dma_poll(&s->dma),A733_CAPTURE_LOCAL_HANDOFF);break;
    case A733_CAPTURE_LOCAL_HANDOFF:
        r=s->config.ops.handoff(s->config.context,s->token,A733_CAPTURE_LOCAL,s,s->deadline);
        if(r==1){s->local_granted=1;next(s,A733_CAPTURE_RECEIVER_OPEN);}break;
    case A733_CAPTURE_RECEIVER_OPEN:
        RUN(a733_csi_open_image(&s->receiver,s->deadline),a733_csi_poll(&s->receiver),A733_CAPTURE_PACKET_HANDOFF);break;
    case A733_CAPTURE_PACKET_HANDOFF:
        if(!s->started){
            if(a733_camera_dma_capture(&s->dma,s->deadline)!=1){r=-1;break;}s->started=1;
        }
        r=s->config.ops.handoff(s->config.context,s->token,A733_CAPTURE_PACKETS,s,s->deadline);
        if(r==1){s->packets_granted=1;next(s,A733_CAPTURE_SENSOR_START);}break;
    case A733_CAPTURE_SENSOR_START:
        RUN(ov5640_start(&s->sensor,s->receiver.token,s->deadline),ov5640_poll(&s->sensor),A733_CAPTURE_DMA_RUN);break;
    case A733_CAPTURE_DMA_RUN:
        r=a733_camera_dma_poll(&s->dma);
        if(s->dma.state==A733_CAM_DMA_STOP)next(s,A733_CAPTURE_SENSOR_STOP);break;
    case A733_CAPTURE_SENSOR_STOP:
        RUN(ov5640_stop(&s->sensor,s->deadline),ov5640_poll(&s->sensor),
            s->retained_readout?A733_CAPTURE_SAMPLE:A733_CAPTURE_QUIET);break;
    case A733_CAPTURE_QUIET:
        r=s->config.ops.packets_quiet(s->config.context,s->token,s->deadline);
        if(r==1){s->quiet=1;next(s,A733_CAPTURE_SAMPLE);}break;
    case A733_CAPTURE_SAMPLE:
        RUN(a733_csi_sample(&s->receiver,s->deadline),a733_csi_poll(&s->receiver),A733_CAPTURE_FENCE);
        if(r==1)s->sampled=1;break;
    case A733_CAPTURE_FENCE:
        r=csi_read(s,s->receiver.token,A733_CSI_PARSER,0xc,&v,s->deadline);
        if(r==1){if(v&0x03030303){r=-1;break;}s->fenced=1;
            next(s,s->retained_readout?A733_CAPTURE_READOUT_ROUTE:A733_CAPTURE_DMA_DISABLE);}break;
    case A733_CAPTURE_READOUT_ROUTE:
        r=route_read(s,s->route.token,A733_ROUTE_VIPP,0x30,&v,s->deadline);
        if(r==1){s->route.events|=v;if(v!=0x100){r=-1;break;}next(s,A733_CAPTURE_IOMMU_SCAN);}break;
    case A733_CAPTURE_DMA_DISABLE:
        r=a733_camera_dma_poll(&s->dma);
        if(s->dma.state==A733_CAM_DMA_DRAIN)next(s,A733_CAPTURE_ROUTE_CLOSE);break;
    case A733_CAPTURE_ROUTE_CLOSE:
        RUN(a733_camera_route_finish(&s->route,s->deadline),a733_camera_route_poll(&s->route),A733_CAPTURE_RECEIVER_CLOSE);break;
    case A733_CAPTURE_RECEIVER_CLOSE:
        RUN(a733_csi_close(&s->receiver,s->deadline),a733_csi_poll(&s->receiver),A733_CAPTURE_SENSOR_CLOSE);break;
    case A733_CAPTURE_SENSOR_CLOSE:
        RUN(ov5640_close(&s->sensor,s->deadline),ov5640_poll(&s->sensor),A733_CAPTURE_DRAIN);break;
    case A733_CAPTURE_DRAIN:
        r=s->config.dma.drain(s->config.context,s->dma.token,s->deadline);
        if(r==1){s->drained=1;next(s,A733_CAPTURE_IOMMU_SCAN);}break;
    case A733_CAPTURE_IOMMU_SCAN:
        RUN(a733_iommu_scan(&s->iommu,s->deadline),a733_iommu_poll(&s->iommu),A733_CAPTURE_DMA_FINISH);
        if(r==1)s->scanned=1;break;
    case A733_CAPTURE_DMA_FINISH:
        if(s->retained_readout && !s->started){
            if(a733_camera_dma_readout(&s->dma,s->deadline)!=1){r=-1;break;}s->started=1;
        }
        r=a733_camera_dma_poll(&s->dma);
        if(s->retained_readout && s->dma.state==A733_CAM_DMA_READABLE)next(s,A733_CAPTURE_READABLE);
        if(s->dma.state==A733_CAM_DMA_READY)next(s,A733_CAPTURE_DMA_CLOSE);break;
    case A733_CAPTURE_DMA_CLOSE:
        RUN(a733_camera_dma_close(&s->dma,s->deadline),a733_camera_dma_poll(&s->dma),A733_CAPTURE_READY);break;
    default:r=-1;break;
    }
    if(r<0 || r>1 || s->state==A733_CAPTURE_FAULT){fail(s);goto out;}
    if(tick(s)!=1)goto out;
out:
    s->entered=0;
    return s->state==A733_CAPTURE_FAULT?-1:
        (s->state==A733_CAPTURE_READY || s->state==A733_CAPTURE_READABLE)?1:0;
}
#undef RUN
