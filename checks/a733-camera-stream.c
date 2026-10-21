/* SPDX-License-Identifier: GPL-2.0-only */
/* Continuous capture composition model. The native coordinator, sensor,
 * analog, IOMMU, route, receiver and DMA controllers run against modeled
 * registers. A frame engine latches the DMA address registers at each frame
 * start, raises the start flag, writes one frame's payload into the latched
 * Y and UV slots at frame end and raises frame done. Device and CPU memory
 * are separate; slot invalidation copies one slot to the CPU view. */
#include "a733_camera_capture.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SLOTS 3u
#define SLOT_BYTES 81920u /* 320x240 plane rounded up to whole pages. */
#define SPAN (SLOTS*SLOT_BYTES)
#define Y_IOVA 0x48100000u
#define UV_IOVA 0x48200000u
static _Alignas(16384) uint32_t table[8192];
static _Alignas(4096) uint8_t parameters[4096],y_cpu[SPAN],uv_cpu[SPAN];
static uint8_t y_device[SPAN],uv_device[SPAN];
struct model {
    struct a733_camera_capture *capture;
    uint32_t common[0x2000/4],parser[2][1024],top[256],vipp[256],dma[1024],mmu[0x20000/4];
    uint8_t sensor[65536];
    uint64_t now,next_start,frame_end,period,active;
    uint32_t latched[2];
    unsigned frames,writing,plane_bytes,global_held,local,packets,quiet,syncs,final_syncs;
    unsigned releases,drains,race,race_middle,missing_done,error_frame,fail_slot_sync;
    unsigned delivered,checked_frames;
};
static struct model model;
static struct a733_camera_capture capture;
static uint8_t pixel(unsigned frame,unsigned plane,unsigned i){
    return plane?(uint8_t)(i*7+9+frame*101):(uint8_t)(i*17+3+frame*101);
}
static int streaming(const struct model *m){
    unsigned port=m->capture->config.receiver.port;
    return m->sensor[0x300e]==0x45 && !m->sensor[0x4202] && (m->parser[port][3]&3)==2 &&
        (m->dma[0]&1) && (m->dma[0x200/4]&1) && (m->common[(0x1000+port*0x400)/4]&1);
}
static unsigned slot_of(const struct model *m,unsigned plane,uint32_t field){
    uint64_t address=(uint64_t)field<<2,base=plane?UV_IOVA:Y_IOVA;
    assert(address>=base && (address-base)%SLOT_BYTES==0 && (address-base)/SLOT_BYTES<SLOTS);
    (void)m;return (unsigned)((address-base)/SLOT_BYTES);
}
/* The device never writes a slot that the CPU may read or that no address
 * register named when the frame started. */
static void writable(const struct model *m,unsigned slot){
    uint8_t state=m->capture->dma.ring.state[slot];
    assert(state==A733_SLOT_NEXT || state==A733_SLOT_ACTIVE);
}
static void frame_start(struct model *m){
    m->latched[0]=m->dma[0x220/4];m->latched[1]=m->dma[0x228/4];
    m->dma[0x254/4]|=0x80;m->writing=1;m->frame_end=m->next_start+m->active;
    m->next_start+=m->period;
}
static void frame_end(struct model *m){
    for(unsigned plane=0;plane<2;plane++){
        unsigned slot=slot_of(m,plane,m->latched[plane]);writable(m,slot);
        uint8_t *d=(plane?uv_device:y_device)+slot*SLOT_BYTES;
        for(unsigned i=0;i<m->plane_bytes;i++)d[i]=pixel(m->frames,plane,i);
    }
    ++m->frames;m->writing=0;
    if(!m->missing_done)m->dma[0x254/4]|=2;
    if(m->error_frame && m->frames==m->error_frame)m->dma[0x254/4]|=0x80000; /* Pixel miss. */
}
static void advance(struct model *m){
    for(;;){
        if(m->writing && m->frame_end<=m->now){frame_end(m);continue;}
        if(!m->writing && streaming(m) && m->next_start<=m->now){frame_start(m);continue;}
        if(!streaming(m) && m->next_start<=m->now)m->next_start=m->now+m->period;
        return;
    }
}
static int now(void *p,uint64_t *v){struct model *m=p;advance(m);*v=m->now;return 1;}
static int global_acquire(void *p,const struct a733_camera_capture_config *c,uint64_t d,uint64_t *t){
    struct model *m=p;assert(c==&m->capture->config && m->now<d);*t=101;m->global_held=1;return 1;
}
static int handoff(void *p,uint64_t t,enum a733_capture_handoff h,const struct a733_camera_capture *s,uint64_t d){
    struct model *m=p;assert(t==101 && m->now<d && s==m->capture);
    assert(s->route.state==A733_ROUTE_ARMED && s->sensor.state==OV5640_READY);
    if(h==A733_CAPTURE_LOCAL){assert(s->dma.state==A733_CAM_DMA_ARMED);m->local=1;return 1;}
    /* Video capture mode and a streaming DMA ring precede packets. */
    assert(m->local && s->receiver.state==A733_CSI_ARMED && !s->receiver.image_mode);
    assert(s->dma.state==A733_CAM_DMA_STREAM_START && (m->parser[s->config.receiver.port][3]&3)==2);
    m->packets=1;return 1;
}
static int packet_quiet(void *p,uint64_t t,uint64_t d){
    struct model *m=p;assert(t==101 && m->now<d && m->sensor[0x300e]==0x40 && m->sensor[0x4202]==15);
    if(m->writing)return 0; /* A frame in flight ends before quiet holds. */
    m->quiet=1;return 1;
}
static int analog_admit(void *p,uint64_t d,struct a733_camera_admission *a){
    struct model *m=p;assert(m->global_held && m->now<d);
    *a=(struct a733_camera_admission){201,A733_CAMERA_REQUIRED_PROOF,A733_CAMERA_COLD_RESET};return 1;
}
static int analog_read(void *p,uint64_t t,uint16_t o,uint32_t *v,uint64_t d){
    struct model *m=p;assert(t==201 && o<0x2000 && m->now<d);
    if(o==4 && (m->common[1]&2))m->common[1]=(m->common[1]&0x1f7)|8|((m->common[1]&0x1f0)<<12);
    *v=m->common[o/4];return 1;
}
static int analog_write(void *p,uint64_t t,uint16_t o,uint32_t v,uint64_t d){
    struct model *m=p;assert(t==201 && (o==0 || o==4) && m->now<d && !m->local);m->common[o/4]=v;return 1;
}
static int mmu_admit(void *p,const struct a733_iommu_tables *x,uint64_t d,struct a733_iommu_admission *a){
    struct model *m=p;assert(m->global_held && m->now<d && x->count==3 && x->words==table);
    *a=(struct a733_iommu_admission){.token=202,.excluded_masters=A733_IOMMU_REAL_MASTERS,
        .powered_masters=A733_IOMMU_REAL_MASTERS,.inherited_resolved=1,.transactions_drained=1,
        .clocks_held=1,.recovery_qualified=1};return 1;
}
static int mmu_read(void *p,uint64_t t,uint32_t o,uint32_t *v,uint64_t d){
    struct model *m=p;assert(t==202 && o<0x20000 && m->now<d);*v=m->mmu[o/4];return 1;
}
static int mmu_write(void *p,uint64_t t,uint32_t o,uint32_t v,uint64_t d){
    struct model *m=p;assert(t==202 && o<0x20000 && m->now<d && !m->packets);m->mmu[o/4]=v;return 1;
}
static int mmu_publish(void *p,uint64_t t,const void *x,size_t n,uint64_t d){
    struct model *m=p;assert(t==202 && x==table && n==sizeof(table) && m->now<d);return 1;
}
static int sensor_acquire(void *p,uint64_t d,uint64_t *t){struct model *m=p;assert(m->now<d);*t=203;return 1;}
static int sensor_line(void *p,uint64_t t,enum ov5640_line l,int a,uint64_t d){
    struct model *m=p;(void)l;assert(t==203 && !a && m->now<d);return 1;
}
static int sensor_read(void *p,uint64_t t,uint8_t a,uint16_t r,uint8_t *v,uint64_t d){
    struct model *m=p;assert(t==203 && a==0x3c && m->now<d);*v=m->sensor[r];return 1;
}
static int sensor_write(void *p,uint64_t t,uint8_t a,uint16_t r,uint8_t v,uint64_t d){
    struct model *m=p;assert(t==203 && a==0x3c && m->now<d);m->sensor[r]=v;return 1;
}
static int sensor_receiver(void *p,uint64_t t,uint64_t rt,const struct ov5640_mode *mode,uint64_t d){
    struct model *m=p;assert(t==203 && rt==206 && m->packets && m->now<d);
    assert(mode->width==m->capture->config.mode.width && mode->height==m->capture->config.mode.height);
    return 1;
}
static int sensor_release(void *p,uint64_t t,uint64_t d){
    struct model *m=p;assert(t==203 && m->now<d && m->quiet);++m->releases;return 1;
}
static int route_admit(void *p,const struct a733_route_config *c,uint64_t d,uint64_t *t){
    struct model *m=p;assert(c->parameter_address==0x48000000 && m->now<d);*t=204;return 1;
}
static int route_read(void *p,uint64_t t,enum a733_route_region r,uint16_t o,uint32_t *v,uint64_t d){
    struct model *m=p;assert(t==204 && o<1024 && m->now<d);*v=(r?m->vipp:m->top)[o/4];return 1;
}
static int route_write(void *p,uint64_t t,enum a733_route_region r,uint16_t o,uint32_t v,uint64_t d){
    struct model *m=p;assert(t==204 && o<1024 && m->now<d);
    uint32_t *reg=(r?m->vipp:m->top)+o/4;
    /* Capture disable completes at once; parameters stay loaded. */
    if(r && o==0x30)*reg&=~v;else *reg=v;
    if(r && o==0x40 && (v&4))m->vipp[0x30/4]=0x100;
    return 1;
}
static int route_publish(void *p,uint64_t t,const uint8_t x[1024],uint64_t d){
    struct model *m=p;assert(t==204 && m->now<d);memcpy(parameters,x,1024);return 1;
}
static int route_drain(void *p,uint64_t t,uint64_t d){
    struct model *m=p;assert(t==204 && m->now<d && m->quiet && !(m->dma[0]&1));return 1;
}
static int route_release(void *p,uint64_t t,uint64_t d){struct model *m=p;assert(t==204 && m->now<d);++m->releases;return 1;}
static uint32_t *csi_reg(struct model *m,enum a733_csi_region r,uint16_t o){
    unsigned port=m->capture->config.receiver.port;
    if(r==A733_CSI_PARSER)return &m->parser[port][o/4];
    return &m->common[(o+(r==A733_CSI_PHY?0x100+port*0x100:r==A733_CSI_PORT?0x1000+port*0x400:0))/4];
}
static int csi_admit(void *p,const struct a733_csi_config *c,uint64_t d,uint64_t *t){
    struct model *m=p;assert(m->local && c->port==c->parser && m->now<d);*t=206;return 1;
}
static int csi_read(void *p,uint64_t t,enum a733_csi_region r,uint16_t o,uint32_t *v,uint64_t d){
    struct model *m=p;assert(t==206 && m->now<d);*v=*csi_reg(m,r,o);return 1;
}
static int csi_write(void *p,uint64_t t,enum a733_csi_region r,uint16_t o,uint32_t v,uint64_t d){
    struct model *m=p;assert(t==206 && m->now<d && m->local && r!=A733_CSI_TOP);
    /* Video capture stops only after the sensor stop and packet quiet. */
    if(m->packets && r==A733_CSI_PARSER && o==0xc)assert(!v && m->quiet && m->sensor[0x4202]==15);
    uint32_t *reg=csi_reg(m,r,o);
    if((r==A733_CSI_PORT && o==0x118)||(r==A733_CSI_PARSER && o==0x44))*reg&=~v;else *reg=v;
    return 1;
}
static int csi_drain(void *p,uint64_t t,uint64_t d){struct model *m=p;assert(t==206 && m->now<d && m->quiet);return 1;}
static int csi_release(void *p,uint64_t t,uint64_t d){struct model *m=p;assert(t==206 && m->now<d);++m->releases;return 1;}
static int dma_admit(void *p,const struct a733_camera_dma_config *c,uint64_t d,uint64_t *t){
    struct model *m=p;assert(c->block==0 && c->frame_id==19 && m->now<d && !m->packets);
    assert(c->y.bus_address==Y_IOVA && c->y.mapped_bytes==SPAN && c->uv.bus_address==UV_IOVA);
    *t=205;return 1;
}
static int dma_read(void *p,uint64_t t,uint16_t o,uint32_t *v,uint64_t d){
    struct model *m=p;assert(t==205 && o<4096 && m->now<d);*v=m->dma[o/4];return 1;
}
static int dma_write(void *p,uint64_t t,uint16_t o,uint32_t v,uint64_t d){
    struct model *m=p;assert(t==205 && o<4096 && m->now<d);
    /* A frame start between the acknowledgement and an address write. */
    if(m->race && m->frames>=m->race && o==(m->race_middle?0x228:0x220)){
        /* A stalled owner: the current frame ends and the next one starts
         * before this write lands. */
        m->race=0;if(m->writing)frame_end(m);m->next_start=m->now;frame_start(m);
    }
    if(o==0x254)m->dma[o/4]&=~v;else m->dma[o/4]=v;
    if(m->packets && o==0x200 && !(v&1)){
        /* Capture disable ends DMA writes; it follows the producer fence. */
        assert(m->quiet && !m->writing);m->dma[0x24c/4]=0;
    }
    return 1;
}
static int dma_drain(void *p,uint64_t t,uint64_t d){
    struct model *m=p;assert(t==205 && m->now<d && m->quiet && !(m->dma[0x200/4]&1));++m->drains;return 1;
}
static int dma_sync(void *p,uint64_t t,uint64_t d){
    struct model *m=p;assert(t==205 && m->now<d && m->drains && m->capture->scanned);
    memcpy(y_cpu,y_device,SPAN);memcpy(uv_cpu,uv_device,SPAN);++m->final_syncs;return 1;
}
static int dma_sync_slot(void *p,uint64_t t,unsigned slot,uint64_t d){
    struct model *m=p;assert(t==205 && m->now<d && slot<SLOTS);
    /* Neither address register nor the latched frame names the slot. */
    assert(slot_of(m,0,m->dma[0x220/4])!=slot && slot_of(m,1,m->dma[0x228/4])!=slot);
    assert(!m->writing || (slot_of(m,0,m->latched[0])!=slot && slot_of(m,1,m->latched[1])!=slot));
    if(m->fail_slot_sync)return -1;
    memcpy(y_cpu+slot*SLOT_BYTES,y_device+slot*SLOT_BYTES,SLOT_BYTES);
    memcpy(uv_cpu+slot*SLOT_BYTES,uv_device+slot*SLOT_BYTES,SLOT_BYTES);
    ++m->syncs;return 1;
}
static int dma_release(void *p,uint64_t t,uint64_t d){struct model *m=p;assert(t==205 && m->now<d);++m->releases;return 1;}
static int setup_mode(unsigned port,enum ov5640_format format,unsigned slots,uint32_t slot_bytes,uint64_t y_bytes){
    memset(&model,0,sizeof(model));memset(&capture,0,sizeof(capture));model.capture=&capture;model.now=1;
    model.common[0]=0x360500;model.common[1]=0xa5;
    for(unsigned i=1;i<=3;i++)model.common[i*0x100/4]=0x40000;
    model.sensor[0x300a]=0x56;model.sensor[0x300b]=0x40;
    memset(y_cpu,0xcc,SPAN);memset(uv_cpu,0xcc,SPAN);memset(y_device,0xa5,SPAN);memset(uv_device,0xa5,SPAN);
    struct ov5640_mode mode;assert(ov5640_mode_get(&mode,OV5640_QVGA,format,0)==1);
    model.plane_bytes=(unsigned)mode.width*mode.height;
    model.period=(uint64_t)mode.htotal*mode.vtotal*1000000u/mode.pixel_hz;
    model.active=model.period*mode.height/mode.vtotal;
    struct a733_camera_capture_config c={
        .ops={now,global_acquire,handoff,packet_quiet},.context=&model,
        .memory={{parameters,0x48000000,0x50000000,4096},{y_cpu,Y_IOVA,0x50100000,y_bytes},{uv_cpu,UV_IOVA,0x50200000,SPAN}},
        .table_words=table,.table_physical=0x4f000000,.frame_id=19,
        .receiver={(uint8_t)port,(uint8_t)port,42,(uint8_t)(port?7:2),324000000},.mode=mode,
        .slots=(uint8_t)slots,.slot_bytes=slot_bytes,.color_bars=1,
        .sensor={.acquire=sensor_acquire,.line=sensor_line,.read=sensor_read,.write=sensor_write,.receiver=sensor_receiver,.release=sensor_release},
        .analog={.admit=analog_admit,.read=analog_read,.write=analog_write},
        .iommu={.admit=mmu_admit,.read=mmu_read,.write=mmu_write,.publish=mmu_publish},
        .route={.admit=route_admit,.publish=route_publish,.read=route_read,.write=route_write,.drain=route_drain,.release=route_release},
        .csi={.admit=csi_admit,.read=csi_read,.write=csi_write,.drain=csi_drain,.release=csi_release},
        .dma={.admit=dma_admit,.read=dma_read,.write=dma_write,.drain=dma_drain,.sync_cpu=dma_sync,.release=dma_release,
              .sync_slot=dma_sync_slot}
    };
    return a733_camera_capture_init(&capture,&c);
}
static void setup(unsigned port,enum ov5640_format format){
    assert(setup_mode(port,format,SLOTS,SLOT_BYTES,SPAN)==1);
    assert(a733_camera_capture_open_stream(&capture,600000000)==1);
}
static int poll(void){
    int r=a733_camera_capture_poll(&capture);model.now+=1000;return r;
}
/* A delivered frame holds one complete frame, the one its sequence names. */
static void check_frame(unsigned slot,uint32_t sequence,const uint8_t *y,const uint8_t *uv){
    assert(y==y_cpu+slot*SLOT_BYTES && uv==uv_cpu+slot*SLOT_BYTES && sequence>=1);
    for(unsigned i=0;i<model.plane_bytes;i++)
        assert(y[i]==pixel(sequence-1,0,i) && uv[i]==pixel(sequence-1,1,i));
    ++model.checked_frames;
}
static void accounting(void){
    const struct a733_camera_ring *g=&capture.dma.ring;
    assert(!g->starts || g->starts==g->delivered+g->dropped+1);
}
static int syncing(void){
    for(unsigned k=0;k<SLOTS;k++)if(capture.dma.ring.state[k]==A733_SLOT_SYNC)return 1;
    return 0;
}
/* Run until stopped after `frames` starts, taking every frame at once or
 * holding up to `hold` frames for `keep` polls each. */
static void run(unsigned frames,unsigned hold,unsigned keep){
    struct { unsigned slot,age; } held[SLOTS];unsigned count=0;
    while(capture.state!=A733_CAPTURE_STREAMING)assert(poll()>=0);
    while(capture.dma.ring.starts<frames){
        assert(poll()==1);accounting();
        for(unsigned i=0;i<count;)
            if(++held[i].age>=keep){assert(a733_camera_capture_give(&capture,held[i].slot)==1);held[i]=held[--count];}
            else ++i;
        unsigned slot;uint32_t sequence;const uint8_t *y,*uv;
        while(count<hold && a733_camera_capture_take(&capture,&slot,&sequence,&y,&uv)==1){
            check_frame(slot,sequence,y,uv);held[count].slot=slot;held[count++].age=0;
        }
        if(!hold)while(a733_camera_capture_take(&capture,&slot,&sequence,&y,&uv)==1){
            check_frame(slot,sequence,y,uv);assert(a733_camera_capture_give(&capture,slot)==1);
        }
    }
    /* Finish the invalidation of the last delivered frame and take it. */
    for(unsigned i=0;i<8 && (capture.dma.index || syncing());i++)assert(poll()==1);
    assert(!capture.dma.index && !syncing());
    if(!hold){
        unsigned slot;uint32_t sequence;const uint8_t *y,*uv;
        while(a733_camera_capture_take(&capture,&slot,&sequence,&y,&uv)==1){
            check_frame(slot,sequence,y,uv);assert(a733_camera_capture_give(&capture,slot)==1);
        }
    }
    if(count)assert(a733_camera_capture_stop(&capture,model.now+100000000)==-1);
    while(count)assert(a733_camera_capture_give(&capture,held[--count].slot)==1);
}
static void stop(void){
    assert(a733_camera_capture_stop(&capture,model.now+100000000)==1);
    int r=0;for(unsigned i=0;i<20000 && !r;i++)r=poll();
    assert(r==1 && capture.state==A733_CAPTURE_READY && model.final_syncs==1);
    assert(capture.iommu.admission.token==202 && !capture.dma.token && !capture.sensor.token);
    assert(model.releases==4 && model.sensor[0x4202]==15 && !(model.parser[capture.config.receiver.port][3]&3));
    /* Both complete spans now belong to the caller. */
    assert(!memcmp(y_cpu,y_device,SPAN) && !memcmp(uv_cpu,uv_device,SPAN));
}
int main(void){
    /* Every frame delivered when the consumer keeps up, on both ports and
     * in both YUV422 orders, at 320x240. */
    for(unsigned port=0;port<2;port++){
        setup(port,port?OV5640_YUYV:OV5640_UYVY);run(12,0,0);
        const struct a733_camera_ring *g=&capture.dma.ring;
        assert(g->starts==12 && g->dropped==0 && model.checked_frames==11 && model.syncs==11);
        assert(capture.receiver.format.width==320 && capture.dma.format.order==port);
        stop();
    }
    /* A consumer holding two slots of three leaves none free: the active
     * slot repeats, each repeated frame is dropped and counted. */
    setup(0,OV5640_UYVY);run(20,2,70);
    assert(capture.dma.ring.dropped>0 && model.checked_frames>0);
    assert(capture.dma.ring.starts==capture.dma.ring.delivered+capture.dma.ring.dropped+1);
    stop();
    /* A frame start racing either address write delivers nothing until the
     * following start; every later frame keeps its sequence and content. */
    for(unsigned middle=0;middle<2;middle++){
        setup(0,OV5640_UYVY);model.race=4;model.race_middle=middle;run(12,0,0);
        assert(!model.race && capture.dma.ring.dropped==2 && model.checked_frames==9);
        stop();
    }
    /* Without frame-done observations no frame is delivered. */
    setup(1,OV5640_UYVY);model.missing_done=1;run(8,0,0);
    assert(!model.checked_frames && !model.syncs && capture.dma.ring.dropped==7);
    stop();
    /* An error status, a failed invalidation or a stop with a held slot
     * keeps every lease; a fault ends all I/O. */
    setup(0,OV5640_UYVY);model.error_frame=3;
    while(poll()>=0)assert(model.now<100000000);
    assert(capture.state==A733_CAPTURE_FAULT && capture.dma.state==A733_CAM_DMA_FAULT);
    assert(capture.dma.token==205 && capture.sensor.token==203 && capture.receiver.token==206 && !model.releases);
    setup(0,OV5640_UYVY);model.fail_slot_sync=1;
    while(poll()>=0)assert(model.now<100000000);
    assert(!model.syncs && capture.dma.token==205 && !model.releases);
    /* Refused configurations. */
    assert(setup_mode(0,OV5640_UYVY,1,SLOT_BYTES,SPAN)==-1);
    assert(setup_mode(0,OV5640_UYVY,5,SLOT_BYTES,SPAN)==-1);
    assert(setup_mode(0,OV5640_UYVY,SLOTS,SLOT_BYTES-8192,SPAN)==-1); /* Below one plane. */
    assert(setup_mode(0,OV5640_UYVY,SLOTS,SLOT_BYTES+100,SPAN)==-1);
    assert(setup_mode(0,OV5640_UYVY,SLOTS,SLOT_BYTES,SPAN-4096)==-1);
    assert(setup_mode(0,OV5640_SBGGR8,SLOTS,SLOT_BYTES,SPAN)==-1);
    /* A still-image open of a ring configuration keeps the SCAP path, and a
     * single-image configuration cannot stream. */
    assert(setup_mode(0,OV5640_UYVY,0,0,SPAN)==1 && a733_camera_capture_open_stream(&capture,1000000)==-1);
    puts("PASS camera streaming: ring delivery on both ports and orders, dropped-frame accounting, "
         "start races, missing frame done, faults and refusals");
    return 0;
}
