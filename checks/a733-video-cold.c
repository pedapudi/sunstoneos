/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Reuse unchanged JPEG fixture/model helpers; main below selects only the
 * composed cold-start behavior. The inherited-handoff suite is not executed. */
#pragma GCC diagnostic push
/* Renaming the unused C main removes its implicit return0 rule. */
#pragma GCC diagnostic ignored "-Wreturn-type"
#define main inherited_main
#include "a733-video-platform.c"
#undef main
#pragma GCC diagnostic pop
struct cold_model {
    struct model m;
    uint32_t pck[1024];
    uint8_t rom[45056];
    unsigned fault,rom_reads,polls,cold_pulse;
};
static int cold_read(void *v,uintptr_t a,uint32_t *out)
{
    struct cold_model *c=v;struct model *m=&c->m;
    if(a>=0x17000&&a<0x22000) {
        assert(m->lock&&!m->writes);++c->rom_reads;
        memcpy(out,c->rom+(a-0x17000),4);return 1;
    }
    if(a>=PCK&&a<PCK+0x1000) {
        assert(m->lock);*out=c->pck[(a-PCK)/4];
        if(a==PCK+8&&c->pck[0]==8) {
            assert(!(m->ccu[0xa8c/4]&(1u<<18))||c->pck[2]==8);
            if(++c->polls>=3&&c->fault!=3)c->pck[2]=*out=8;
        }
        return 1;
    }
    if(a>=VE&&a<VE+sizeof(m->ve)) {
        assert(c->rom_reads==15&&c->pck[2]==8);
        assert((m->ccu[0xa8c/4]&0x40004)==0x40004);
        assert(m->ccu[0xa88/4]&0x80000000);
        if(a==VE+0xe0||a==VE+0xe4||a==VE+0xf0)assert(c->cold_pulse==2);
    }
    return read_reg(v,a,out);
}
static int cold_write(void *v,uintptr_t a,unsigned width,uint32_t value)
{
    struct cold_model*c=v;struct model*m=&c->m;
    assert(m->lock&&c->rom_reads==15&&width==4);
    if(m->memory)return write_reg(v,a,width,value);
    assert(m->writes<800);
    m->events[m->writes++]=(struct event){a,width,value};
    if(c->fault==4)return -1;
    if(a>=PCK&&a<PCK+0x1000) {
        assert(!(m->ccu[0xa8c/4]&0x40004));
        assert(!(m->ccu[0xa88/4]&0x80000000));
        c->pck[(a-PCK)/4]=value;
    }else if(a==VE+0x804) {
        assert(c->pck[2]==8&&(m->ccu[0xa88/4]&0x80000000));
        assert(!m->memory&&c->cold_pulse<2);
        assert((value&0x11)==(c->cold_pulse?0:0x11));
        assert(!((value^m->ve[0x804/4])&~0x11));
        m->ve[0x804/4]=value;++c->cold_pulse;
    }else {
        assert(a>=CCU&&a<CCU+sizeof(m->ccu)&&c->pck[2]==8);
        uint32_t mask=a==CCU+0xa8c?0x40004:a==CCU+0xa88?0x80000000:
            a==CCU+0x5c0?1:a==CCU+0x5e0?0x4000:a==CCU+0x5e4?0x40000:0;
        assert(mask&&!((m->ccu[(a-CCU)/4]^value)&~mask));
        m->ccu[(a-CCU)/4]=value;
        if(a==CCU+0xa88)m->ccu[0x1b10/4]=0x20000;
    }
    return 1;
}
static int cold_codec_write(void*v,uintptr_t a,unsigned width,uint32_t value)
{
    struct cold_model*c=v;
    return c->m.memory?write_reg(v,a,width,value):cold_write(v,a,width,value);
}
int main(int argc,char **argv)
{
    assert(argc==3||(argc==4&&!strcmp(argv[3],"--reset")));uint8_t bytes[65536];
    FILE*f=fopen(argv[1],"rb");assert(f);size_t n=fread(bytes,1,sizeof(bytes),f);assert(n&&n<sizeof(bytes));fclose(f);
    for(unsigned test=0;test<6;++test) {
        if(argc==4&&test)continue;
        struct cold_model c={.fault=test};struct model*m=&c.m;
        setup(m,argc==4?UINT64_C(0x3331000021320):UINT64_C(0x3332100021321));
        f=fopen(argv[2],"rb");assert(f);assert(fread(c.rom,1,sizeof(c.rom),f)==sizeof(c.rom));assert(fgetc(f)==EOF);fclose(f);
        m->ccu[0xa88/4]&=~0x80000000u;m->ccu[0xa8c/4]=0x10001;
        m->ccu[0x5c0/4]=0xa5000302;m->ccu[0x5e0/4]=0xb1001003;m->ccu[0x5e4/4]=0x102;
        m->ccu[0x1b10/4]=0x10000;
        if(test==1)c.rom[0x2ca0]^=1;
        if(test==2)m->ccu[0x1998/4]=0;
        if(test==5)m->ve[0xe0/4]=0xdead;
        struct a733_clock_leases clocks={0};assert(a733_clock_leases_init(&clocks,now,m));
        struct a733_gpu_resource_manager power={0};
        struct a733_gpu_resource_ops po={now,read_any,unused_update,unused_rail};
        assert(a733_gpu_resources_init(&power,&po,m));
        struct a733_gpu_power_resources ready;uint64_t token=0;
        assert(a733_gpu_resources_acquire(&power,2,A733_GPU_RESOURCE_OSC|A733_GPU_RESOURCE_PCK,100000,&token,&ready)==A733_GPU_RESOURCE_OK);
        struct a733_video_platform_config config={.clocks=&clocks,.power=&power,.power_client=2,.owner=99,.handoff=123,
            .pck={token,A733_GPU_RESOURCE_OSC|A733_GPU_RESOURCE_PCK,A733_GPU_RESOURCE_STABLE},
            .module=0x80000001,.module_hz=234000000,.fabric={0,0x80000000,0x80000000},
            .fabric_hz={24000000,24000000,24000000},.start=A733_VIDEO_COLD_NS_ROM};
        uint32_t scopes;assert(a733_video_platform_scopes(&config,&scopes));
        assert(a733_clock_acquire(&clocks,99,scopes,A733_CLOCK_VE_DEC,100000,&config.clock)==A733_CLOCK_OK);
        struct a733_clock_token conflict={0};
        assert(a733_clock_acquire(&clocks,100,scopes,A733_CLOCK_VE_DEC,100000,&conflict)!=A733_CLOCK_OK);
        struct a733_video_platform_ops ops={now,admit,memory,drain,enter,leave,cold_read,cold_codec_write};
        struct a733_video_platform p={0};assert(a733_video_platform_init(&p,&config,&ops,&c));
        struct a733_video_jpeg_job job={bytes,n,0x81000000,65536,0x82000000,2048,0x83000000,1024};
        struct a733_video_jpeg_run run={0};assert(a733_video_jpeg_begin(&run,a733_video_platform_jpeg_ops(),&p,&job,100000));
        int result=0;unsigned polls=0;
        while(!result&&++polls<20000)result=a733_video_jpeg_poll(&run);
        assert(polls<20000&&run.token==42);
        if(!test) {
            assert(result==1&&m->drain==2&&m->trigger&&!m->lock);
            assert(m->events[0].address==PCK+0x170&&m->events[5].address==PCK);
            assert(m->events[6].address==CCU+0xa8c&&m->events[11].address==CCU+0xa88);
            assert(m->events[12].address==VE+0x804&&m->events[12].value==0x11);
            assert(m->events[13].address==VE+0x804&&m->events[13].value==0);
            assert(c.cold_pulse==2);
            if(argc==4)assert(m->events[15].address==VE+4&&m->events[16].address==VE+4);
            assert(m->ccu[0x5c0/4]==0xa5000303&&m->ccu[0x5e0/4]==0xb1005003&&m->ccu[0x5e4/4]==0x40102);
        }else {
            assert(result==-1&&!m->trigger&&!m->memory);
            if(test==1||test==2)assert(!m->writes&&!m->vereads);
            if(test==3)assert(m->writes==6&&!(m->ccu[0xa8c/4]&(1u<<18))&&!m->vereads);
            if(test==4)assert(m->writes==1&&p.locked&&m->lock&&!m->vereads);
            if(test==5)assert(m->writes==14&&m->vereads);
            unsigned accesses=m->reads+m->writes;
            assert(a733_video_jpeg_poll(&run)==-1&&accesses==m->reads+m->writes);
        }
        assert(clocks.slots[0].id==config.clock.id&&power.leases[2].token==token);
        printf("case%u result%d cold_writes%u ROM_reads%u drain%u\n",test,result,test?m->writes:14,c.rom_reads,m->drain);
    }
    puts("PASS: immutable-ROM cold admission, reset held through power acknowledgement, preserved counterpart gates, JPEG completion with independent drain, retained failures. Model only.");
}
