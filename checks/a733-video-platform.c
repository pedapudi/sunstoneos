/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "a733_video_platform.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CCU 0x02002000u
#define VE 0x01c0e000u
#define PCK 0x07062000u
#define GIC 0x03400000u
struct event {uint32_t address,width,value;};
struct model {
    uint32_t ccu[2048],ve[1024],gic[1024];
    uint64_t now,reset_at,version;
    unsigned lock,writes,reads,vereads,admissions,memory,drain,trigger,fail;
    struct event events[800];
};
static int now(void *v,uint64_t *n) {struct model*m=v;*n=++m->now;return 1;}
static int read_any(void*v,uintptr_t a,uint32_t*out)
{
    struct model*m=v;++m->reads;
    if(a==0x07090160)*out=0;
    else if(a==0x070101ac)*out=1;
    else if(a==PCK||a==PCK+8)*out=m->fail==2?0:8;
    else if(a>=CCU&&a<CCU+sizeof(m->ccu))*out=m->ccu[(a-CCU)/4];
    else if(a>=GIC&&a<GIC+sizeof(m->gic))*out=m->gic[(a-GIC)/4];
    else if(a>=VE&&a<VE+sizeof(m->ve)) {
        assert(m->lock);++m->vereads;*out=m->ve[(a-VE)/4];
        if(a==VE+0x11c&&m->trigger)*out=1;
    }else assert(0);
    return 1;
}
static int read_reg(void*v,uintptr_t a,uint32_t*out)
{struct model*m=v;assert(m->lock);return read_any(v,a,out);}
static int write_reg(void*v,uintptr_t a,unsigned width,uint32_t value)
{
    struct model*m=v;assert(m->lock&&m->memory&&m->writes<800);
    m->events[m->writes++]=(struct event){(uint32_t)a,width,value};
    if(m->fail==6)return -1;
    if(a==CCU+0xa8c) {
        assert(width==4);assert(!((value^m->ccu[0xa8c/4])&~(1u<<18)));
        if(!(value&(1u<<18)))m->reset_at=m->now;
        else assert(m->reset_at&&m->now>=m->reset_at+10);
        m->ccu[0xa8c/4]=value;
    }else {
        assert(a>=VE&&a<VE+sizeof(m->ve));
        if(width==4)m->ve[(a-VE)/4]=value;
        else ((uint8_t*)m->ve)[a-VE]=(uint8_t)value;
        if(a==VE+0x118)m->trigger=1;
    }
    if(m->fail==7)m->now=100000;
    return 1;
}
static int unused_update(void*v,uintptr_t a,uint32_t required,uint32_t set,uint64_t d)
{(void)v;(void)a;(void)required;(void)set;(void)d;assert(0);return 0;}
static int unused_rail(void*v,struct a733_gpu_resource_rail*r,uint64_t d)
{(void)v;(void)r;(void)d;assert(0);return 0;}
static int admit(void*v,const struct a733_video_platform_config*c,
 const struct a733_video_jpeg_job*j,uint64_t d,uint64_t*t)
{struct model*m=v;(void)j;(void)d;assert(c->owner==99&&c->handoff==123&&!m->lock);++m->admissions;*t=42;return 1;}
static int memory(void*v,uint64_t t,const struct a733_video_jpeg_job*j,
 const struct a733_video_jpeg*p,uint32_t stride,uint32_t rows,uint64_t d)
{struct model*m=v;(void)j;(void)d;assert(t==42&&!m->lock&&p->width==48&&stride==64&&rows==32);++m->memory;return m->fail==5?-1:1;}
static int drain(void*v,uint64_t t,uint64_t d)
{struct model*m=v;(void)d;assert(t==42&&!m->lock&&m->trigger);++m->drain;return m->fail==8?-1:(m->drain>1);}
static int enter(void*v,uint64_t d)
{struct model*m=v;(void)d;assert(!m->lock);m->lock=1;return 1;}
static int leave(void*v)
{struct model*m=v;assert(m->lock);m->lock=0;return 1;}
static void setup(struct model*m,uint64_t version)
{
    m->version=version;
    m->ccu[0]=0xf8000000u|(99u<<8)|(99u<<16);
    m->ccu[0x240/4]=0xf8000000u|(77u<<8)|(3u<<20);
    m->ccu[0x1918/4]=0x20002;m->ccu[0x1998/4]=0x20000;m->ccu[0x1b10/4]=0x20000;
    m->ccu[0xa88/4]=0x80000001;
    m->ccu[0xa8c/4]=0x40004;
    m->ccu[0x580/4]=m->ccu[0x588/4]=0x80000000;
    m->ccu[0x5c0/4]=1;m->ccu[0x5e0/4]=1u<<14;m->ccu[0x5e4/4]=1u<<18;
    for(unsigned i=0;i<1024;++i)m->ve[i]=0x80808080;
    m->ve[0]=m->ve[1]=m->ve[0x804/4]=0;
    m->ve[0xe0/4]=(uint32_t)(version>>32);m->ve[0xe4/4]=(uint32_t)version;
    m->ve[0xf0/4]=m->ve[0x11c/4]=0;
}
int main(int argc,char**argv)
{
    assert(argc==3||(argc==4&&!strcmp(argv[3],"--gates")));FILE*f=fopen(argv[1],"rb");assert(f);
    uint8_t *bytes=calloc(1,65536);assert(bytes);size_t n=fread(bytes,1,65536,f);assert(n&&n<65536&&!ferror(f));fclose(f);
    const uint64_t ids[]={UINT64_C(0x3332100021321),UINT64_C(0x3331000021320),UINT64_C(0x3052000012011)};
    for(unsigned test=0;test<17;++test) {
        if(argc==4&&test>0&&test<12)continue;
        unsigned fail=(test<3||test==16)?0:test-2;uint64_t version=ids[test<3?test:0];
        struct model m={.fail=fail};setup(&m,version);
        struct a733_clock_leases clocks={0};assert(a733_clock_leases_init(&clocks,now,&m));
        struct a733_gpu_resource_manager power={0};struct a733_gpu_resource_ops po={now,read_any,unused_update,unused_rail};
        assert(a733_gpu_resources_init(&power,&po,&m));
        struct a733_gpu_power_resources ready;uint64_t power_token=0;
        assert(a733_gpu_resources_acquire(&power,2,A733_GPU_RESOURCE_OSC|A733_GPU_RESOURCE_PCK,100000,&power_token,&ready)==A733_GPU_RESOURCE_OK);
        struct a733_video_platform_config c={.clocks=&clocks,.power=&power,.power_client=2,.owner=99,.handoff=123,
            .pck={power_token,A733_GPU_RESOURCE_OSC|A733_GPU_RESOURCE_PCK,A733_GPU_RESOURCE_STABLE},
            .module=0x80000001,.module_hz=234000000,.fabric={0,0x80000000,0x80000000},.fabric_hz={24000000,24000000,24000000}};
        if(fail==11||test==16) {
            c.fabric[0]=0x03000000;c.fabric[1]=0x83000000;c.fabric[2]=0x81000000;
            for(unsigned i=0;i<3;++i)c.fabric_hz[i]=600000000;
            m.ccu[0x500/4]=c.fabric[0];m.ccu[0x580/4]=c.fabric[1];m.ccu[0x588/4]=c.fabric[2];
            m.ccu[0xa0/4]=0xfe000000|(99u<<8)|(1u<<20)|(2u<<16)|(4u<<2);
            m.ccu[0x1908/4]=0x0fff0fff;m.ccu[0x1988/4]=0x0fff0000;
        }
        a733_clock_mask_t scopes;assert(a733_video_platform_scopes(&c,&scopes));
        assert(a733_clock_acquire(&clocks,99,scopes,A733_CLOCK_VE_DEC,100000,&c.clock)==A733_CLOCK_OK);
        struct a733_video_platform_ops ops={now,admit,memory,drain,enter,leave,read_reg,write_reg};
        struct a733_video_platform p={0};assert(a733_video_platform_init(&p,&c,&ops,&m));
        if(fail==1)m.ccu[0x1918/4]&=~2u; /* Auto mode is not No-Auto. */
        if(fail==3)m.ve[0xe0/4]=0xdead;
        if(fail==4)m.gic[0x308/4]=1u<<17;
        if(fail==10)m.ccu[0x1998/4]=0;
        if(fail==11)m.ccu[0x1988/4]&=~(1u<<25);
        if(fail==12)m.ccu[0x1b10/4]|=1;
        if(fail==13)m.ccu[0x1b10/4]=0x10000;
        if(fail==9)++p.config.clock.id; /* Forged token must fail before MMIO. */
        struct a733_video_jpeg_job job={bytes,(uint32_t)n,0x81000000,65536,0x82000000,2048,0x83000000,1024};
        struct a733_video_jpeg_run run={0};
        assert(a733_video_jpeg_begin(&run,a733_video_platform_jpeg_ops(),&p,&job,100000));
        int result=0;unsigned polls=0,saved=0,before=m.reads;
        while(!result&&++polls<2000) {
            result=a733_video_jpeg_poll(&run);
            if(!fail&&p.prepared&&!saved) {
                char path[1024];assert(snprintf(path,sizeof(path),"%s/%u.bin",argv[2],test)>0);
                f=fopen(path,"wb");assert(f);assert(fwrite(m.events,sizeof(m.events[0]),m.writes,f)==m.writes);fclose(f);saved=1;
            }
        }
        assert(polls<2000&&run.token==42&&p.token==42);
        if(!fail)assert(result==1&&m.drain==2&&m.trigger&&!m.lock&&saved);
        else {
            assert(result==-1);
            if(fail<=4||fail==9||fail>=10)assert(!m.memory&&!m.writes);
            if(fail==1||fail==2||fail==4||fail==9||fail>=10)assert(!m.vereads);
            if(fail==5)assert(m.memory&&!m.writes);
            if(fail==6||fail==7)assert(m.lock&&p.locked&&!m.trigger);
            if(fail==8)assert(m.trigger&&m.drain==1);
            if(fail==9)assert(m.reads==before);
            unsigned accesses=m.reads+m.writes;
            assert(a733_video_jpeg_poll(&run)==-1&&m.reads+m.writes==accesses);
        }
        assert(clocks.slots[0].id==c.clock.id&&power.leases[2].token==power_token);
    }
    free(bytes);puts("PASS: native VE binding model, real token authentication, identity-before-reset, reset delay, retained faults and independent drain. No devices accessed.");
}
