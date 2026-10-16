/* SPDX-License-Identifier: GPL-2.0-only */
#include "a733_camera_capture.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define PLANE 307200u
#define SPAN (PLANE+4096u)
static _Alignas(16384) uint32_t table[8192];
static _Alignas(4096) uint8_t parameters[4096],y_cpu[SPAN],uv_cpu[SPAN];
static uint8_t y_device[SPAN],uv_device[SPAN];
struct model {
    struct a733_camera_capture *capture;
    uint32_t common[0x2000/4],parser[2][1024],top[256],vipp[256],dma[1024],mmu[0x20000/4];
    uint8_t sensor[65536];
    uint64_t now;
    unsigned calls,writes,frames,attempts,cache_syncs,route_drains,receiver_drains,frame_drains;
    unsigned releases,global_held,local,packets,quiet,device_prepared,table_published,parameters_published;
    unsigned fault_kind,injected,pending_local,pending_packets,pending_quiet,pending_drain;
    enum a733_capture_state fault_state;
    unsigned fail_emit,stuck_capture,stuck_fence,receiver_error,route_error,iommu_error;
    unsigned fail_sync,late_release,failed_global,changed_global,ready_clock_fault;
};
static struct model model;
static struct a733_camera_capture capture;
static int operation(struct model *m){
    ++m->calls;
    if(!m->injected && m->fault_kind && m->capture->state==m->fault_state){
        m->injected=1;
        if(m->fault_kind==1)return -1;
        if(m->fault_kind==2)m->now=m->capture->deadline;
        if(m->fault_kind==3)assert(a733_camera_capture_poll(m->capture)==-1);
    }
    return 1;
}
static int now(void *p,uint64_t *v){
    struct model *m=p;
    if(m->ready_clock_fault && m->capture->state==A733_CAPTURE_READY){
        if(m->ready_clock_fault==1)assert(a733_camera_capture_poll(m->capture)==-1);
        else m->now=m->capture->deadline;
    }
    *v=m->now;return 1;
}
static int global_acquire(void *p,const struct a733_camera_capture_config *c,uint64_t d,uint64_t *t){
    struct model *m=p;assert(c==&m->capture->config && m->now<d);
    *t=m->changed_global && m->global_held?102:101;m->global_held=1;
    int r=operation(m);if(m->failed_global)return -1;
    return m->changed_global && *t==101?0:r;
}
static int handoff(void *p,uint64_t t,enum a733_capture_handoff h,const struct a733_camera_capture *s,uint64_t d){
    struct model *m=p;assert(t==101 && m->global_held && m->now<d && s==m->capture);
    assert(s->analog.admission.token==201 && s->analog.state==A733_CAMERA_PLATFORM_ACTIVE);
    assert(s->iommu.admission.token==202 && s->iommu.state==A733_IOMMU_ACTIVE);
    assert(s->iommu.admission.excluded_masters==A733_IOMMU_REAL_MASTERS);
    assert(s->route.state==A733_ROUTE_ARMED && s->sensor.state==OV5640_READY);
    assert(m->table_published && m->parameters_published && m->device_prepared && !m->frames);
    int r=operation(m);if(r!=1)return r;
    if(h==A733_CAPTURE_LOCAL){
        assert(s->dma.state==A733_CAM_DMA_ARMED && s->receiver.state==A733_CSI_NEW);
        if(m->pending_local){--m->pending_local;return 0;}m->local=1;
    }else{
        assert(m->local && s->receiver.state==A733_CSI_ARMED && s->receiver.image_mode);
        assert(s->dma.state==A733_CAM_DMA_START && (m->parser[s->config.receiver.parser][3]&3)==1);
        if(m->pending_packets){--m->pending_packets;return 0;}m->packets=1;
    }return 1;
}
static int packet_quiet(void *p,uint64_t t,uint64_t d){
    struct model *m=p;assert(t==101 && m->now<d && m->capture->sensor.state==OV5640_READY);
    assert(m->sensor[0x300e]==0x40 && m->sensor[0x4202]==15);
    int r=operation(m);if(r!=1)return r;
    if(m->pending_quiet){--m->pending_quiet;return 0;}m->quiet=1;return 1;
}
static int analog_admit(void *p,uint64_t d,struct a733_camera_admission *a){
    struct model *m=p;assert(m->global_held && !m->local && m->now<d);
    *a=(struct a733_camera_admission){201,A733_CAMERA_REQUIRED_PROOF,A733_CAMERA_COLD_RESET};
    return operation(m);
}
static int analog_read(void *p,uint64_t t,uint16_t o,uint32_t *v,uint64_t d){
    struct model *m=p;assert(t==201 && o<0x2000 && m->now<d);
    if(o==4 && (m->common[1]&2))m->common[1]=(m->common[1]&0x1f7)|8|((m->common[1]&0x1f0)<<12);
    *v=m->common[o/4];return operation(m);
}
static int analog_write(void *p,uint64_t t,uint16_t o,uint32_t v,uint64_t d){
    struct model *m=p;assert(t==201 && (o==0 || o==4) && m->now<d && !m->local);
    if(o==4)assert(!(v&~0x1f7u));m->common[o/4]=v;++m->writes;return operation(m);
}
static int mmu_admit(void *p,const struct a733_iommu_tables *x,uint64_t d,struct a733_iommu_admission *a){
    struct model *m=p;assert(m->global_held && m->now<d && x->count==3 && x->words==table);
    *a=(struct a733_iommu_admission){202,A733_IOMMU_REAL_MASTERS,A733_IOMMU_REAL_MASTERS,1,1,1,1};
    return operation(m);
}
static int mmu_read(void *p,uint64_t t,uint32_t o,uint32_t *v,uint64_t d){
    struct model *m=p;assert(t==202 && o<0x20000 && !(o&3) && m->now<d);
    *v=m->mmu[o/4];return operation(m);
}
static int mmu_write(void *p,uint64_t t,uint32_t o,uint32_t v,uint64_t d){
    struct model *m=p;assert(t==202 && o<0x20000 && !(o&3) && m->now<d && !m->frames);
    m->mmu[o/4]=v;++m->writes;return operation(m);
}
static int mmu_publish(void *p,uint64_t t,const void *x,size_t n,uint64_t d){
    struct model *m=p;assert(t==202 && x==table && n==sizeof(table) && m->now<d);
    m->table_published=1;return operation(m);
}
static int sensor_acquire(void *p,uint64_t d,uint64_t *t){struct model *m=p;assert(m->global_held && m->now<d);*t=203;return operation(m);}
static int sensor_line(void *p,uint64_t t,enum ov5640_line l,int a,uint64_t d){
    struct model *m=p;assert(t==203 && (l==0 || l==1) && !a && m->now<d && !m->packets);return operation(m);
}
static int sensor_read(void *p,uint64_t t,uint8_t a,uint16_t r,uint8_t *v,uint64_t d){
    struct model *m=p;assert(t==203 && a==0x3c && m->now<d);*v=m->sensor[r];return operation(m);
}
static void emit(struct model *m){
    struct a733_camera_capture *s=m->capture;unsigned port=s->config.receiver.port;
    ++m->attempts;
    if(!(m->parser[port][3]&1))return;
    assert(m->packets && m->local && m->device_prepared && m->parameters_published);
    assert((m->common[0]&0x60000f0f)==0xb0f && (m->common[(0x100+port*0x100)/4]&1));
    assert(m->common[(0x1000+port*0x400)/4]&1);
    assert((m->top[0x40/4]&15)==port*4 && (m->top[0xa0/4]&15)==4);
    assert((m->vipp[0]&3)==3 && (m->vipp[0x40/4]&1) && (m->dma[0]&1) && (m->dma[0x200/4]&1));
    assert(m->mmu[0x14/4]==1 && m->mmu[0x10014/4]==1 && m->mmu[0x604/4]==0);
    for(unsigned id=0;id<12;id++){
        const struct a733_iommu_master *master=a733_iommu_master(id);
        if(master && id!=1)assert(m->mmu[(master->micro_offset+4)/4]==1);
    }
    uint64_t physical;
    assert(a733_iommu_translate(&s->tables,(uint64_t)m->vipp[0x50/4]<<2,A733_IOMMU_READ,&physical)==1);
    assert(physical==s->config.memory[0].physical);
    for(unsigned i=0;i<2;i++){
        uint64_t address=(uint64_t)m->dma[(i?0x228:0x220)/4]<<2;
        assert(a733_iommu_translate(&s->tables,address,A733_IOMMU_WRITE,&physical)==1);
        assert(physical==s->config.memory[i+1].physical);
        assert(a733_iommu_translate(&s->tables,address+PLANE-1,A733_IOMMU_WRITE,&physical)==1);
        assert(physical==s->config.memory[i+1].physical+PLANE-1);
    }
    for(unsigned i=0;i<PLANE;i++){y_device[i]=(uint8_t)(i*17+3);uv_device[i]=(uint8_t)(i*7+9);}
    ++m->frames;
    if(!m->stuck_fence)m->parser[port][3]&=~1u;
    m->parser[port][0x44/4]=1;
    m->common[(0x1118+port*0x400)/4]=m->receiver_error?0x80:3;
    m->vipp[0x40/4]&=~4u;m->vipp[0x30/4]=m->route_error?0x101:0x100;
    m->dma[0x254/4]=m->stuck_capture?2:3;
}
static int sensor_write(void *p,uint64_t t,uint8_t a,uint16_t r,uint8_t v,uint64_t d){
    struct model *m=p;assert(t==203 && a==0x3c && m->now<d);m->sensor[r]=v;++m->writes;
    if(r==0x4202 && !v && m->sensor[0x300e]==0x45){
        emit(m);if(m->fail_emit){operation(m);return -1;}
    }return operation(m);
}
static int sensor_receiver(void *p,uint64_t t,uint64_t rt,const struct ov5640_mode *modes,uint64_t d){
    struct model *m=p;assert(t==203 && rt==206 && m->packets && m->now<d);
    assert(modes->width==640 && modes->height==480 && modes->lanes==2);return operation(m);
}
static int sensor_release(void *p,uint64_t t,uint64_t d){
    struct model *m=p;assert(t==203 && m->now<d && m->capture->receiver.state==A733_CSI_CLOSED);
    assert(m->capture->route.state==A733_ROUTE_CLOSED && m->quiet);++m->releases;return operation(m);
}
static int route_admit(void *p,const struct a733_route_config *c,uint64_t d,uint64_t *t){
    struct model *m=p;assert(c->parameter_address==0x48000000 && m->now<d && !m->packets);*t=204;return operation(m);
}
static int route_read(void *p,uint64_t t,enum a733_route_region r,uint16_t o,uint32_t *v,uint64_t d){
    struct model *m=p;assert(t==204 && o<1024 && m->now<d);*v=(r?m->vipp:m->top)[o/4];return operation(m);
}
static int route_write(void *p,uint64_t t,enum a733_route_region r,uint16_t o,uint32_t v,uint64_t d){
    struct model *m=p;assert(t==204 && o<1024 && m->now<d);
    if(m->frames)assert(m->quiet && !(m->dma[0x200/4]&1));
    uint32_t *reg=(r?m->vipp:m->top)+o/4;
    if(r && o==0x30)*reg&=~v;else *reg=v;++m->writes;return operation(m);
}
static int route_publish(void *p,uint64_t t,const uint8_t x[1024],uint64_t d){
    struct model *m=p;assert(t==204 && m->now<d && !m->packets && m->table_published);
    memcpy(parameters,x,1024);m->parameters_published=1;return operation(m);
}
static int route_drain(void *p,uint64_t t,uint64_t d){
    struct model *m=p;assert(t==204 && m->now<d && m->quiet && !(m->vipp[0]&3) && !(m->dma[0]&1));
    ++m->route_drains;return operation(m);
}
static int route_release(void *p,uint64_t t,uint64_t d){
    struct model *m=p;assert(t==204 && m->now<d && m->route_drains && m->global_held);++m->releases;return operation(m);
}
static uint32_t *csi_reg(struct model *m,enum a733_csi_region r,uint16_t o){
    unsigned port=m->capture->config.receiver.port;
    if(r==A733_CSI_PARSER)return &m->parser[port][o/4];
    return &m->common[(o+(r==A733_CSI_PHY?0x100+port*0x100:r==A733_CSI_PORT?0x1000+port*0x400:0))/4];
}
static int csi_admit(void *p,const struct a733_csi_config *c,uint64_t d,uint64_t *t){
    struct model *m=p;assert(m->local && !m->packets && c->port==c->parser && m->now<d);*t=206;return operation(m);
}
static int csi_read(void *p,uint64_t t,enum a733_csi_region r,uint16_t o,uint32_t *v,uint64_t d){
    struct model *m=p;assert(t==206 && m->now<d);*v=*csi_reg(m,r,o);return operation(m);
}
static int csi_write(void *p,uint64_t t,enum a733_csi_region r,uint16_t o,uint32_t v,uint64_t d){
    struct model *m=p;assert(t==206 && m->now<d && m->local && r!=A733_CSI_TOP);
    uint32_t *reg=csi_reg(m,r,o);
    if((r==A733_CSI_PORT && o==0x118)||(r==A733_CSI_PARSER && o==0x44))*reg&=~v;
    else *reg=v;++m->writes;return operation(m);
}
static int csi_drain(void *p,uint64_t t,uint64_t d){
    struct model *m=p;assert(t==206 && m->now<d && m->quiet && m->capture->route.state==A733_ROUTE_CLOSED);
    assert(!(*csi_reg(m,A733_CSI_PORT,0)&1) && !(*csi_reg(m,A733_CSI_PHY,0)&1));
    ++m->receiver_drains;return operation(m);
}
static int csi_release(void *p,uint64_t t,uint64_t d){
    struct model *m=p;assert(t==206 && m->now<d && m->receiver_drains && m->global_held);++m->releases;return operation(m);
}
static int dma_admit(void *p,const struct a733_camera_dma_config *c,uint64_t d,uint64_t *t){
    struct model *m=p;assert(c->block==0 && c->frame_id==19 && m->now<d && !m->packets);
    *t=205;m->device_prepared=1;return operation(m);
}
static int dma_read(void *p,uint64_t t,uint16_t o,uint32_t *v,uint64_t d){
    struct model *m=p;assert(t==205 && o<4096 && m->now<d);
    /* The sensor may offer another frame before stop; SCAP must reject it. */
    if(o==0x254 && m->frames && m->attempts==1 && !m->stuck_fence)emit(m);
    *v=m->dma[o/4];return operation(m);
}
static int dma_write(void *p,uint64_t t,uint16_t o,uint32_t v,uint64_t d){
    struct model *m=p;assert(t==205 && o<4096 && m->now<d);
    if(m->frames && (o==0 || o==0x200))assert(m->quiet && m->capture->fenced);
    if(o==0x254)m->dma[o/4]&=~v;else m->dma[o/4]=v;++m->writes;return operation(m);
}
static int dma_drain(void *p,uint64_t t,uint64_t d){
    struct model *m=p;assert(t==205 && m->now<d && m->quiet && m->frames==1);
    assert(m->capture->sensor.state==OV5640_CLOSED && m->capture->receiver.state==A733_CSI_CLOSED);
    assert(m->route_drains && m->receiver_drains && !m->cache_syncs);
    int r=operation(m);if(r!=1)return r;
    if(m->pending_drain){--m->pending_drain;return 0;}
    ++m->frame_drains;if(m->iommu_error)m->mmu[0xb0/4]=1;return 1;
}
static int dma_sync(void *p,uint64_t t,uint64_t d){
    struct model *m=p;assert(t==205 && m->now<d && m->frame_drains==1 && m->capture->scanned);
    assert(m->capture->iommu.fault.complete && m->capture->iommu.state==A733_IOMMU_ACTIVE);
    assert(!m->dma[0x254/4] && m->capture->state!=A733_CAPTURE_READY);
    for(unsigned i=0;i<SPAN;i++)assert(y_cpu[i]==0xcc && uv_cpu[i]==0xcc);
    ++m->cache_syncs;int r=operation(m);if(m->fail_sync)return -1;if(r!=1)return r;
    memcpy(y_cpu,y_device,SPAN);memcpy(uv_cpu,uv_device,SPAN);return 1;
}
static int dma_release(void *p,uint64_t t,uint64_t d){
    struct model *m=p;assert(t==205 && m->now<d && m->cache_syncs && m->global_held);
    ++m->releases;if(m->late_release)m->now=d;return operation(m);
}
static void setup(unsigned port){
    memset(&model,0,sizeof(model));memset(&capture,0,sizeof(capture));model.capture=&capture;model.now=1;
    model.common[0]=0x360500;model.common[1]=0xa5;
    for(unsigned i=1;i<=3;i++)model.common[i*0x100/4]=0x40000;
    model.sensor[0x300a]=0x56;model.sensor[0x300b]=0x40;
    memset(parameters,0xcc,sizeof(parameters));memset(y_cpu,0xcc,sizeof(y_cpu));memset(uv_cpu,0xcc,sizeof(uv_cpu));
    memset(y_device,0xa5,sizeof(y_device));memset(uv_device,0xa5,sizeof(uv_device));
    struct a733_camera_capture_config c={
        .ops={now,global_acquire,handoff,packet_quiet},.context=&model,
        .memory={{parameters,0x48000000,0x50000000,4096},{y_cpu,0x48010000,0x50100000,SPAN},{uv_cpu,0x48080000,0x50200000,SPAN}},
        .table_words=table,.table_physical=0x4f000000,.frame_id=19,
        .receiver={(uint8_t)port,(uint8_t)port,42,(uint8_t)(port?7:2),324000000},.color_bars=(int)port,
        .sensor={.acquire=sensor_acquire,.line=sensor_line,.read=sensor_read,.write=sensor_write,.receiver=sensor_receiver,.release=sensor_release},
        .analog={.admit=analog_admit,.read=analog_read,.write=analog_write},
        .iommu={.admit=mmu_admit,.read=mmu_read,.write=mmu_write,.publish=mmu_publish},
        .route={.admit=route_admit,.publish=route_publish,.read=route_read,.write=route_write,.drain=route_drain,.release=route_release},
        .csi={.admit=csi_admit,.read=csi_read,.write=csi_write,.drain=csi_drain,.release=csi_release},
        .dma={.admit=dma_admit,.read=dma_read,.write=dma_write,.drain=dma_drain,.sync_cpu=dma_sync,.release=dma_release}
    };
    assert(a733_camera_capture_init(&capture,&c)==1);
    assert(a733_camera_capture_open(&capture,10000000)==1);
}
static int drive(void){
    int r=0;
    for(unsigned i=0;i<11000 && !r;i++){
        unsigned n=model.calls;r=a733_camera_capture_poll(&capture);assert(model.calls-n<=1);
        if(!model.cache_syncs)assert(y_cpu[0]==0xcc && uv_cpu[PLANE-1]==0xcc);
        model.now+=1000;
    }
    assert(r);unsigned n=model.calls;uint64_t time=model.now;
    assert(a733_camera_capture_poll(&capture)==r && model.calls==n && model.now==time);
    assert(capture.token==101 && model.global_held);
    assert(a733_camera_capture_open(&capture,model.now+10000000)==-1);
    return r;
}
int main(int argc,char **argv){
    if(argc==2 && !strcmp(argv[1],"publication")){
        for(unsigned k=1;k<=2;k++){
            setup(0);model.ready_clock_fault=k;assert(drive()==-1);
            assert(capture.state==A733_CAPTURE_FAULT && capture.iommu.admission.token==202 && !capture.dma.token);
        }
        puts("PASS: final ownership publication refuses clock reentry and overdue completion while retaining global mappings");
        return 0;
    }
    for(unsigned port=0;port<2;port++){
        setup(port);model.pending_local=model.pending_packets=model.pending_quiet=model.pending_drain=1;
        assert(drive()==1 && capture.state==A733_CAPTURE_READY);
        assert(model.frames==1 && model.attempts==2 && model.cache_syncs==1 && model.releases==4);
        assert(capture.analog.admission.token==201 && capture.iommu.admission.token==202);
        assert(!capture.sensor.token && !capture.receiver.token && !capture.route.token && !capture.dma.token);
        for(unsigned i=0;i<PLANE;i++){assert(y_cpu[i]==(uint8_t)(i*17+3));assert(uv_cpu[i]==(uint8_t)(i*7+9));}
        for(unsigned i=PLANE;i<SPAN;i++)assert(y_cpu[i]==0xa5 && uv_cpu[i]==0xa5);
        for(unsigned i=1024;i<4096;i++)assert(parameters[i]==0xcc);
    }
    /* New composition boundaries only; leaf behavior already has its own evidence. */
    const enum a733_capture_state boundaries[]={A733_CAPTURE_ACQUIRE,A733_CAPTURE_LOCAL_HANDOFF,
        A733_CAPTURE_PACKET_HANDOFF,A733_CAPTURE_QUIET,A733_CAPTURE_RECEIVER_CLOSE,A733_CAPTURE_DRAIN};
    for(unsigned k=1;k<=3;k++)for(unsigned i=0;i<sizeof(boundaries)/sizeof(boundaries[0]);i++){
        setup(0);model.fault_kind=k;model.fault_state=boundaries[i];assert(drive()==-1 && model.injected);
        assert(!model.cache_syncs && capture.state==A733_CAPTURE_FAULT);
    }
    setup(0);model.failed_global=1;assert(drive()==-1 && !model.writes);
    setup(0);model.changed_global=1;assert(drive()==-1 && !model.writes);
    setup(0);model.fail_emit=1;assert(drive()==-1 && model.frames==1 && !model.releases);
    setup(0);model.stuck_capture=1;assert(drive()==-1 && !model.quiet && !model.cache_syncs);
    setup(0);model.stuck_fence=1;assert(drive()==-1 && !model.route_drains && !model.cache_syncs);
    setup(0);model.receiver_error=1;assert(drive()==-1 && !model.route_drains && !model.cache_syncs);
    setup(0);model.route_error=1;assert(drive()==-1 && !model.cache_syncs && capture.route.token==204);
    setup(0);model.iommu_error=1;assert(drive()==-1 && capture.iommu.state==A733_IOMMU_FAULT && !model.cache_syncs);
    setup(0);model.fail_sync=1;assert(drive()==-1 && capture.dma.owner==A733_CAMERA_DEVICE && capture.dma.token==205);
    setup(0);model.late_release=1;assert(drive()==-1 && !capture.dma.token && capture.iommu.admission.token==202);
    setup(0);assert(a733_camera_capture_poll(&capture)==0);model.now=0;assert(drive()==-1 && !model.writes);
    puts("PASS: both camera routes execute all native controllers; one SCAP frame, mapped NV16 bytes, deferred CPU cache ownership, shared leases and composition fault retention");
    return 0;
}
