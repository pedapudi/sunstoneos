/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Record every callback, register access and clock observation made by the VE
 * decoder (JPEG, H.264) and encoder (JPEG, H.264) bindings. The expected
 * digests were recorded from the separate decoder and encoder resource cores
 * before both moved onto the shared a733_video_unit core. Equal digests show
 * that each binding keeps its exact access order, values, lock pattern and
 * deadline checks. The trace files written to the output directory allow a
 * byte comparison. Usage: a733-video-unit <ns-brom.bin> <output-dir> [--record] */
#include "a733_video_platform.h"
#include "a733_video_encode_platform.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CCU 0x02002000u
#define GIC 0x03400000u
#define ROM 0x17000u
static const uint32_t ve_base[2]={0x01c0e000u,0x01c10000u},pck_base[2]={0x07062000u,0x07063000u};
struct event {uint32_t kind,address,value,time;};
struct model {
    uint32_t ccu[2048],pck[2][1024],ve[2][1024],gic[1024],rom[45056/4],osc;
    uint64_t now;
    unsigned lock,logging,writes,events,fail_write,pending[4],status_polls,enter_busy;
    unsigned busy_write,busy_hold;
    struct a733_clock_leases *clocks;
    struct event event[4096];
};
static uint8_t rom_image[45056];
enum {ADMIT,MEMORY,DRAIN,COPY};
static void record(struct model *m,uint32_t kind,uint32_t address,uint32_t value)
{
    if(!m->logging)return;
    assert(m->events<sizeof(m->event)/sizeof(m->event[0]));
    m->event[m->events++]=(struct event){kind,address,value,(uint32_t)m->now};
}
static int pending(struct model *m,unsigned which)
{if(m->pending[which]){--m->pending[which];return 1;}return 0;}
static int now(void *v,uint64_t *n)
{
    struct model *m=v;
    if(m->busy_hold&&!--m->busy_hold)atomic_store_explicit(&m->clocks->busy,0,memory_order_release);
    *n=++m->now;return 1;
}
static uint32_t *cell(struct model *m,uintptr_t a)
{
    if(a>=CCU&&a<CCU+sizeof(m->ccu))return &m->ccu[(a-CCU)/4];
    if(a>=GIC&&a<GIC+sizeof(m->gic))return &m->gic[(a-GIC)/4];
    for(unsigned i=0;i<2;++i) {
        if(a>=ve_base[i]&&a<ve_base[i]+4096)return &m->ve[i][(a-ve_base[i])/4];
        if(a>=pck_base[i]&&a<pck_base[i]+4096)return &m->pck[i][(a-pck_base[i])/4];
    }
    if(a>=ROM&&a<ROM+sizeof(m->rom))return &m->rom[(a-ROM)/4];
    return NULL;
}
static int read_any(void *v,uintptr_t a,uint32_t *out)
{
    struct model *m=v;
    if(a==0x07090160)*out=m->osc;
    else if(a==0x070101ac)*out=1;
    else {
        uint32_t *c=cell(m,a);assert(c&&!(a&3));*out=*c;
        /* Cold power acknowledgement follows the policy after three polls. */
        if(a==pck_base[0]+8&&(m->pck[0][0]&15)==8&&(*out&15)!=8&&++m->status_polls>=3)*c=*out=8;
    }
    record(m,'R',(uint32_t)a,*out);
    return 1;
}
static int read_reg(void *v,uintptr_t a,uint32_t *out)
{struct model *m=v;assert(m->lock);return read_any(v,a,out);}
static int write_reg(void *v,uintptr_t a,unsigned width,uint32_t value)
{
    struct model *m=v;assert(m->lock);
    record(m,'W'|width<<8,(uint32_t)a,value);
    if(++m->writes==m->busy_write) {
        atomic_store_explicit(&m->clocks->busy,1,memory_order_release);m->busy_hold=8;
    }
    if(m->writes==m->fail_write)return -1;
    uint32_t *c=cell(m,a&~(uintptr_t)3);assert(c&&!(a>=ROM&&a<ROM+sizeof(m->rom)));
    if(width==1)((uint8_t *)c)[a&3]=(uint8_t)value;
    else {assert(width==4&&!(a&3));*c=value;}
    if(a==CCU+0xa88&&(value>>31))m->ccu[0x1b10/4]=0x20000;
    return 1;
}
#ifndef SUNSTONE_A733_VIDEO_UNIT_H
/* The separate encoder core used a 32-bit-only write callback. */
static int write32(void *v,uintptr_t a,uint32_t value){return write_reg(v,a,4,value);}
#define ENCODER_WRITE write32
#else
#define ENCODER_WRITE write_reg
#endif
static int enter(void *v,uint64_t d)
{
    struct model *m=v;(void)d;assert(!m->lock);
    if(m->enter_busy){--m->enter_busy;record(m,'E',0,0);return 0;}
    m->lock=1;record(m,'E',1,0);return 1;
}
static int leave(void *v){struct model *m=v;assert(m->lock);m->lock=0;record(m,'L',0,0);return 1;}
static int unused_update(void *v,uintptr_t a,uint32_t r,uint32_t s,uint64_t d)
{(void)v;(void)a;(void)r;(void)s;(void)d;assert(0);return 0;}
static int unused_rail(void *v,struct a733_gpu_resource_rail *r,uint64_t d){(void)v;(void)r;(void)d;assert(0);return 0;}
/* Decoder JPEG callbacks. */
static int jpeg_admit(void *v,const struct a733_video_platform_config *c,const struct a733_video_jpeg_job *j,uint64_t d,uint64_t *t)
{struct model *m=v;(void)c;(void)d;record(m,'A',j?1:0,0);*t=42;return pending(m,ADMIT)?0:1;}
static int jpeg_memory(void *v,uint64_t t,const struct a733_video_jpeg_job *j,const struct a733_video_jpeg *p,uint32_t s,uint32_t r,uint64_t d)
{struct model *m=v;(void)d;record(m,'M',(uint32_t)t|(j&&p?0x100:0),s<<16|r);return pending(m,MEMORY)?0:1;}
static int jpeg_drain(void *v,uint64_t t,uint64_t d)
{struct model *m=v;(void)d;record(m,'D',(uint32_t)t,0);return pending(m,DRAIN)?0:1;}
/* Decoder H.264 callbacks. */
static int h264_admit(void *v,const struct a733_video_platform_config *c,const struct a733_h264_job *j,uint64_t d,uint64_t *t)
{struct model *m=v;(void)c;(void)d;record(m,'A',(uint32_t)j->buffers[0].iova,0);*t=42;return pending(m,ADMIT)?0:1;}
static int h264_memory(void *v,uint64_t t,const struct a733_h264_job *j,uint64_t d)
{struct model *m=v;(void)d;record(m,'M',(uint32_t)t,(uint32_t)j->buffers[1].iova);return pending(m,MEMORY)?0:1;}
static int h264_copied(void *v,uint64_t t,uint64_t d)
{struct model *m=v;(void)d;record(m,'C',(uint32_t)t,0);return pending(m,COPY)?0:1;}
/* Encoder callbacks. */
static int ejpeg_admit(void *v,const struct a733_encode_platform_config *c,const struct a733_jpeg_encode_job *j,uint64_t d,uint64_t *t)
{struct model *m=v;(void)c;(void)d;record(m,'A',(uint32_t)j->input_iova,0);*t=42;return pending(m,ADMIT)?0:1;}
static int ejpeg_memory(void *v,uint64_t t,const struct a733_jpeg_encode_job *j,uint64_t d)
{struct model *m=v;(void)d;record(m,'M',(uint32_t)t,(uint32_t)j->output_iova);return pending(m,MEMORY)?0:1;}
static int ejpeg_copied(void *v,uint64_t t,size_t n,uint64_t d)
{struct model *m=v;(void)d;record(m,'C',(uint32_t)t,(uint32_t)n);return pending(m,COPY)?0:1;}
static int eh264_admit(void *v,const struct a733_encode_platform_config *c,const struct a733_h264_encode_job *j,uint64_t d,uint64_t *t)
{struct model *m=v;(void)c;(void)d;record(m,'A',(uint32_t)j->buffers[0].iova,0);*t=42;return pending(m,ADMIT)?0:1;}
static int eh264_memory(void *v,uint64_t t,const struct a733_h264_encode_job *j,uint64_t d)
{struct model *m=v;(void)d;record(m,'M',(uint32_t)t,(uint32_t)j->buffers[2].iova);return pending(m,MEMORY)?0:1;}
static int eh264_prime(void *v,uint64_t t,unsigned o,size_t c,uint64_t d)
{struct model *m=v;(void)d;record(m,'P',(uint32_t)t,o<<24|(uint32_t)c);return 1;}
static int eh264_copied(void *v,uint64_t t,unsigned o,size_t f,size_t e,uint64_t d)
{struct model *m=v;(void)d;record(m,'C',(uint32_t)t|o<<8,(uint32_t)(e-f));return pending(m,COPY)?0:1;}
static void setup(struct model *m,uint64_t version)
{
    m->osc=0x10000002;
    m->ccu[0]=0xf8000000u|(99u<<8)|(99u<<16);
    m->ccu[0x220/4]=m->ccu[0x240/4]=0xf8000000u|(77u<<8)|(3u<<20);
    m->ccu[0x1918/4]=0x30003;m->ccu[0x1998/4]=0x30000;m->ccu[0x1b10/4]=0x20000;
    m->ccu[0xa80/4]=m->ccu[0xa88/4]=0x80000001;
    m->ccu[0xa8c/4]=0x50005;
    m->ccu[0x580/4]=m->ccu[0x588/4]=0x80000000;
    m->ccu[0x5c0/4]=3;m->ccu[0x5e0/4]=0x5000;m->ccu[0x5e4/4]=0x40002;
    for(unsigned i=0;i<2;++i) {
        for(unsigned j=0;j<1024;++j)m->ve[i][j]=0x80808080;
        m->pck[i][0]=m->pck[i][2]=8;
    }
    m->ve[0][0]=m->ve[0][1]=m->ve[0][0x804/4]=m->ve[0][0xf0/4]=0;
    m->ve[0][0xe0/4]=(uint32_t)(version>>32);m->ve[0][0xe4/4]=(uint32_t)version;
    m->ve[1][0x8f0/4]=0;m->ve[1][0x8e0/4]=0x33310;m->ve[1][0x8e4/4]=0x21320;
    memcpy(m->rom,rom_image,sizeof(m->rom));
}
static void cold(struct model *m)
{
    m->ccu[0xa88/4]&=~0x80000000u;m->ccu[0xa8c/4]=0x10001;
    m->ccu[0x5c0/4]=0xa5000302;m->ccu[0x5e0/4]=0xb1001003;m->ccu[0x5e4/4]=0x102;
    m->ccu[0x1b10/4]=0x10000;m->pck[0][0]=m->pck[0][2]=0;
}
struct resources {struct a733_clock_leases clocks;struct a733_gpu_resource_manager power;uint64_t pck;};
static void resources(struct model *m,struct resources *r)
{
    *r=(struct resources){0};
    assert(a733_clock_leases_init(&r->clocks,now,m));
    struct a733_gpu_resource_ops po={now,read_any,unused_update,unused_rail};
    assert(a733_gpu_resources_init(&r->power,&po,m));
    struct a733_gpu_power_resources ready;
    assert(a733_gpu_resources_acquire(&r->power,2,A733_GPU_RESOURCE_OSC|A733_GPU_RESOURCE_PCK,100000,&r->pck,&ready)==A733_GPU_RESOURCE_OK);
    m->clocks=&r->clocks;
}
#define CALL(m,id,expr) do{int result_=(expr);record(m,'=',id,(uint32_t)result_);}while(0)
#define UNTIL(m,id,expr) do{int result_;unsigned tries_=0;do{result_=(expr);record(m,'=',id,(uint32_t)result_);}while(!result_&&++tries_<64);}while(0)
static const uint64_t D=100000;
/* Decoder cases: 0..2 inherited identities, 3 cold, 4 unknown identity,
 * 5 uncertain write, 6 transient registry contention, 7 retained copy,
 * 8 26 MHz reference, 9 VE0 parent, 10 H.264, 11 H.264 changed job,
 * 12 refused mutex entry, 13 missing local gate. */
static void decoder(struct model *m,unsigned test)
{
    static const uint64_t ids[]={UINT64_C(0x3332100021321),UINT64_C(0x3331000021320),UINT64_C(0x3052000012011)};
    setup(m,test==4?UINT64_C(0xdead00021321):ids[test<3?test:test==10||test==11?1:0]);
    if(test==3)cold(m);
    if(test==8){m->osc=0x983fb0f7;m->ccu[0]=0xf8675f00;}
    if(test==13)m->ccu[0x5e0/4]&=~(1u<<14);
    struct resources r;resources(m,&r);
    struct a733_video_platform_config c={.clocks=&r.clocks,.power=&r.power,.power_client=2,.owner=99,.handoff=123,
        .pck={r.pck,A733_GPU_RESOURCE_OSC|A733_GPU_RESOURCE_PCK,A733_GPU_RESOURCE_STABLE},
        .module=test==9?0x81000001:0x80000001,.module_hz=234000000,.fabric={0,0x80000000,0x80000000},
        .fabric_hz={24000000,24000000,24000000},.start=test==3?A733_VIDEO_COLD_NS_ROM:A733_VIDEO_INHERITED};
    if(test!=3)m->ccu[0xa88/4]=c.module;
    a733_clock_mask_t scopes;assert(a733_video_platform_scopes(&c,&scopes));
    assert(a733_clock_acquire(&r.clocks,99,scopes,A733_CLOCK_VE_DEC,100000,&c.clock)==A733_CLOCK_OK);
    m->logging=1;m->pending[ADMIT]=m->pending[MEMORY]=m->pending[DRAIN]=m->pending[COPY]=1;
    if(test==5)m->fail_write=6;
    if(test==6)m->busy_write=9;
    if(test==12)m->enter_busy=1;
    uint64_t token=0;
    if(test>=10) {
        struct a733_h264_platform_ops o={now,h264_admit,h264_memory,h264_copied,enter,leave,read_reg,write_reg};
        static struct a733_h264_platform p;memset(&p,0,sizeof(p));assert(a733_h264_platform_init(&p,&c,&o,m));
        struct a733_h264_job job={.input=rom_image,.input_bytes=4096,.profile=A733_H264_LEGACY};
        for(unsigned i=0;i<A733_H264_BUFFERS;++i)job.buffers[i]=(struct a733_h264_span){0x10000000u+i*0x100000u,8192};
        const struct a733_h264_ops *ops=a733_video_platform_h264_ops();
        UNTIL(m,1,ops->acquire(&p,&job,D,&token));
        if(test==11)++job.buffers[3].iova;
        UNTIL(m,2,ops->prepare(&p,token,&job,D));
        CALL(m,3,ops->read(&p,token,0xe0,&(uint32_t){0},D));
        CALL(m,4,ops->read(&p,token,0x228,&(uint32_t){0},D));
        CALL(m,5,ops->write(&p,token,0x200,0x1234,D));
        UNTIL(m,6,ops->copied(&p,token,D));
        CALL(m,7,ops->write(&p,token,0x118,1,D));
        CALL(m,8,ops->read(&p,token,0x228,&(uint32_t){0},D));
        if(test==11)UNTIL(m,9,ops->acquire(&p,&job,D,&token));
        return;
    }
    struct a733_video_platform_ops o={now,jpeg_admit,jpeg_memory,jpeg_drain,enter,leave,read_reg,write_reg};
    static struct a733_video_platform p;memset(&p,0,sizeof(p));assert(a733_video_platform_init(&p,&c,&o,m));
    struct a733_video_jpeg_job job={rom_image,1028,0x81000000,65536,0x82000000,2048,0x83000000,1024};
    struct a733_video_jpeg picture={.width=48,.height=32};
    const struct a733_video_jpeg_ops *ops=a733_video_platform_jpeg_ops();
    UNTIL(m,1,ops->acquire(&p,&job,D,&token));
    CALL(m,2,ops->read(&p,token,0xe4,&(uint32_t){0},D));
    UNTIL(m,3,ops->prepare(&p,token,&job,&picture,64,32,D));
    CALL(m,4,ops->read(&p,token,0x11c,&(uint32_t){0},D));
    CALL(m,5,ops->write(&p,token,0x118,1,1,D));
    CALL(m,6,ops->write(&p,token,0x1c0,4,0x55aa,D));
    if(test==7) {
        UNTIL(m,7,a733_video_platform_retained_copy(&p,token,D,h264_copied,m));
    }else UNTIL(m,7,ops->finish(&p,token,D));
    CALL(m,8,ops->write(&p,token,0x200,4,1,D)); /* Outside the JPEG allowlist. */
    CALL(m,9,ops->read(&p,token,0,&(uint32_t){0},D));
}
/* Encoder cases: 0 JPEG, 1 H.264, 2 unknown identity, 3 uncertain write,
 * 4 VE1 parent, 5 H.264 changed job, 6 26 MHz reference, 7 missing local
 * gate, 8 transient registry contention. */
static void encoder(struct model *m,unsigned test)
{
    setup(m,0);
    if(test==2)m->ve[1][0x8e0/4]=0xdead;
    if(test==6){m->osc=0x983fb0f7;m->ccu[0]=0xf8675f00;}
    if(test==7)m->ccu[0x5c0/4]&=~2u;
    struct resources r;resources(m,&r);
    struct a733_encode_platform_config c={.clocks=&r.clocks,.power=&r.power,.power_client=2,.owner=99,.handoff=123,
        .pck={r.pck,A733_GPU_RESOURCE_OSC|A733_GPU_RESOURCE_PCK,A733_GPU_RESOURCE_STABLE},
        .module=test==4?0x81000001:0x80000001,.module_hz=234000000,.fabric={0,0x80000000,0x80000000},
        .fabric_hz={24000000,24000000,24000000}};
    m->ccu[0xa80/4]=c.module;
    a733_clock_mask_t scopes;assert(a733_encode_platform_scopes(&c,&scopes));
    assert(a733_clock_acquire(&r.clocks,99,scopes,A733_CLOCK_VE_ENC,100000,&c.clock)==A733_CLOCK_OK);
    m->logging=1;m->pending[ADMIT]=m->pending[MEMORY]=m->pending[COPY]=1;
    if(test==3)m->fail_write=1;
    if(test==8)m->busy_write=1;
    uint64_t token=0;
    if(test==1||test==5) {
        struct a733_h264_encode_platform_ops o={now,eh264_admit,eh264_memory,eh264_prime,eh264_copied,enter,leave,read_reg,ENCODER_WRITE};
        static struct a733_h264_encode_platform p;memset(&p,0,sizeof(p));assert(a733_h264_encode_platform_init(&p,&c,&o,m));
        struct a733_h264_encode_job job={.input={rom_image,rom_image+4096}};
        for(unsigned i=0;i<A733_H264_ENCODE_BUFFERS;++i)job.buffers[i]=(struct a733_h264_encode_span){0x20000000u+i*0x100000u,4096};
        const struct a733_h264_encode_ops *ops=a733_encode_platform_h264_ops();
        UNTIL(m,1,ops->acquire(&p,&job,D,&token));
        CALL(m,2,ops->acquire(&p,&job,D,&token));
        if(test==5)++job.buffers[4].iova;
        UNTIL(m,3,ops->prepare(&p,token,&job,D));
        CALL(m,4,ops->prime(&p,token,0,4096,D));
        CALL(m,5,ops->read(&p,token,0x804,&(uint32_t){0},D));
        CALL(m,6,ops->write(&p,token,0xb0c,0x77,D));
        UNTIL(m,7,ops->copied(&p,token,1,0,4096,D));
        CALL(m,8,ops->write(&p,token,0xb00,0x77,D)); /* Outside the H.264 allowlist. */
        CALL(m,9,ops->read(&p,token,0x804,&(uint32_t){0},D));
        return;
    }
    struct a733_encode_platform_ops o={now,ejpeg_admit,ejpeg_memory,ejpeg_copied,enter,leave,read_reg,ENCODER_WRITE};
    static struct a733_encode_platform p;memset(&p,0,sizeof(p));assert(a733_encode_platform_init(&p,&c,&o,m));
    struct a733_jpeg_encode_job job={rom_image,4096,0x30000000,0x30100000,462848,1048576};
    const struct a733_jpeg_encode_ops *ops=a733_encode_platform_jpeg_ops();
    UNTIL(m,1,ops->acquire(&p,&job,D,&token));
    CALL(m,2,ops->acquire(&p,&job,D,&token));
    UNTIL(m,3,ops->prepare(&p,token,&job,D));
    CALL(m,4,ops->read(&p,token,0x800,&(uint32_t){0},D));
    CALL(m,5,ops->read(&p,token,0xb1c,&(uint32_t){0},D));
    CALL(m,6,ops->write(&p,token,0xa00,0x99,D));
    UNTIL(m,7,ops->copied(&p,token,625,D));
    CALL(m,8,ops->write(&p,token,0xb0c,0x99,D)); /* H.264-only register. */
    CALL(m,9,ops->read(&p,token,0x800,&(uint32_t){0},D));
}
static uint64_t digest(const struct model *m)
{
    uint64_t h=UINT64_C(0xcbf29ce484222325);const uint8_t *b=(const uint8_t *)m->event;
    for(size_t i=0;i<m->events*sizeof(m->event[0]);++i){h^=b[i];h*=UINT64_C(0x100000001b3);}
    return h;
}
/* Pre-consolidation digests: 14 decoder cases, then 9 encoder cases. */
static const uint64_t expected[23]={
    UINT64_C(0xcd48e6b5d119cf94),UINT64_C(0x9570a97a2185b9ed),UINT64_C(0x37b9ca203660eefa),UINT64_C(0x47d928fa9568989e),
    UINT64_C(0x129476d1ed7320eb),UINT64_C(0x0f6231335c732b08),UINT64_C(0x17fbb0afe502d887),UINT64_C(0xd9233c2ce66e13c4),
    UINT64_C(0x51b2844f7ada1f3e),UINT64_C(0xcd2b59379f1def7f),UINT64_C(0x3344d50143d6177b),UINT64_C(0xd43834c74df8e9dc),
    UINT64_C(0xe9b2d34ec9aa9ddf),UINT64_C(0x0c8cb23e4ed1b22b),UINT64_C(0x82c0cd44548146db),UINT64_C(0xbdbca072475f4291),
    UINT64_C(0x9f8a97ac00923d2a),UINT64_C(0x91e9b5cc7c427928),UINT64_C(0x1af598e0f7271864),UINT64_C(0xefd2cdd576676097),
    UINT64_C(0x7be7275832c496f5),UINT64_C(0x9162831735be307b),UINT64_C(0x7272c70023938ee3)
};
int main(int argc,char **argv)
{
    assert(argc==3||(argc==4&&!strcmp(argv[3],"--record")));
    FILE *f=fopen(argv[1],"rb");assert(f);assert(fread(rom_image,1,sizeof(rom_image),f)==sizeof(rom_image));fclose(f);
    unsigned mismatches=0;
    for(unsigned i=0;i<23;++i) {
        static struct model m;memset(&m,0,sizeof(m));
        if(i<14)decoder(&m,i);else encoder(&m,i-14);
        uint64_t h=digest(&m);char path[1024];
        assert(snprintf(path,sizeof(path),"%s/%s%u.bin",argv[2],i<14?"decoder":"encoder",i<14?i:i-14)>0);
        f=fopen(path,"wb");assert(f);assert(fwrite(m.event,sizeof(m.event[0]),m.events,f)==m.events);fclose(f);
        printf("%s%u events%u digest 0x%016llx%s\n",i<14?"decoder":"encoder",i<14?i:i-14,m.events,
            (unsigned long long)h,argc==4||h==expected[i]?"":" MISMATCH");
        if(argc==3&&h!=expected[i])++mismatches;
    }
    fflush(stdout);assert(!mismatches);
    puts(argc==4?"RECORDED: VE binding traces":"PASS: decoder and encoder VE bindings reproduce the recorded resource, register and callback traces. Model only.");
}
