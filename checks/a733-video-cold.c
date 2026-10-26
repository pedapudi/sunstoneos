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
    /* PLL_VE0 programming: the scenario, the table writes in order, reads of
     * the enabled PLL, LOCK observations and when the third was made. */
    unsigned pll,pll_writes,pll_reads,pll_locked;
    uint64_t pll_lock_at,pll_read_at;
};
#define PLL_LOCKS 1u /* Scenario: LOCK appears after two reads. */
#define PLL_NEVER 2u /* Scenario: LOCK never appears. */
#define PLL_DROP 3u  /* Scenario: LOCK appears, drops for one read, returns. */
static int pll_read(struct cold_model *c,uint32_t *out)
{
    struct model *m=&c->m;uint32_t v=m->ccu[0x220/4]&~(1u<<28);
    if((v&0xa0000000u)==0xa0000000u) {
        /* LOCK samples after the lock-enable readback are 3 us apart. */
        if(++c->pll_reads>2&&c->pll_writes==4)assert(m->now>=c->pll_read_at+3);
        c->pll_read_at=m->now;
        if(c->pll_reads>2&&c->pll!=PLL_NEVER&&!(c->pll==PLL_DROP&&c->pll_reads==4))v|=1u<<28;
    }else if(m->ccu[0x220/4]&(1u<<28))v|=1u<<28;
    /* Consecutive LOCK reads before the output gate opens. */
    if(c->pll_writes==4&&c->pll_locked<3) {
        c->pll_locked=(v&(1u<<28))?c->pll_locked+1:0;
        if(c->pll_locked==3)c->pll_lock_at=m->now;
    }
    m->ccu[0x220/4]=v;*out=v;return 1;
}
/* In the PLL cases the binding's clock advances 1 us per eight calls, so
 * that only its explicit waits, and not the time checks around each access,
 * space the LOCK reads and the settle. */
static int cold_now(void *v,uint64_t *n)
{
    struct cold_model *c=v;
    if(!c->pll)return now(v,n);
    static unsigned calls;
    if(++calls%8==0)++c->m.now;
    *n=c->m.now;return 1;
}
static int cold_read(void *v,uintptr_t a,uint32_t *out)
{
    struct cold_model *c=v;struct model *m=&c->m;
    if(a>=0x17000&&a<0x22000) {
        assert(m->lock&&!m->writes);++c->rom_reads;
        memcpy(out,c->rom+(a-0x17000),4);return 1;
    }
    if(a==CCU+0x220&&c->pll){assert(m->lock);++m->reads;return pll_read(c,out);}
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
    }else if(a==CCU+0x220) {
        /* The table of the manual's enable method 2, in order, with the module
         * gate closed; bit 24 kept and LOCK written as zero. */
        static const uint32_t set[5]={1u<<30,0,1u<<31,1u<<29,1u<<27};
        uint32_t old=m->ccu[0x220/4];
        assert(c->pll&&c->pck[2]==8&&c->pll_writes<5&&!(m->ccu[0xa88/4]&0x80000000));
        assert((m->ccu[0xa88/4]&0x0700001f)==0x01000000);
        assert(!(value&(1u<<28))&&!((value^old)&~0xf870ff02u&~(1u<<28)));
        if(c->pll_writes==1)assert((value&0x0070ff02u)==0x00204d00u&&!((value^old)&~0x0070ff02u&~(1u<<28)));
        else assert(!((value^old)&~(1u<<28)&~set[c->pll_writes])&&(value&set[c->pll_writes]));
        if(c->pll_writes==4)assert(c->pll_locked==3&&m->now>=c->pll_lock_at+20);
        m->ccu[0x220/4]=value|(old&(1u<<28));++c->pll_writes;
    }else {
        assert(a>=CCU&&a<CCU+sizeof(m->ccu)&&c->pck[2]==8);
        uint32_t mask=a==CCU+0xa8c?0x40004:a==CCU+0xa88?(c->pll&&!(m->ccu[0xa88/4]&0x80000000)?0x8700001f:0x80000000):
            a==CCU+0x5c0?1:a==CCU+0x5e0?0x4000:a==CCU+0x5e4?0x40000:0;
        assert(mask&&!((m->ccu[(a-CCU)/4]^value)&~mask));
        if(a==CCU+0xa88&&(value&0x80000000)&&c->pll)assert(c->pll_writes==5||(m->ccu[0x220/4]&0xf8000000u)==0xf8000000u);
        m->ccu[(a-CCU)/4]=value;
        if(a==CCU+0xa88&&(value&0x80000000))m->ccu[0x1b10/4]=0x20000;
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
    /* Cases 7-13 start at the vendor operating point with PLL_VE0 as the
     * port 3-3 clock dump read it at FEL entry (board-session-20261025-24,
     * clock-audit-b): disabled at 0x41305a00, gate controls 0x00030003 and
     * status 0x00030000, module word 0. 7 programs it, 8 does so on the
     * 26 MHz board, 9 adopts it running, 10 refuses other running factors,
     * 11 never sees LOCK, 12 refuses a clear gate control and 13 sees LOCK
     * drop once before three LOCK reads in a row. */
    for(unsigned test=0;test<14;++test) {
        if(argc==4&&test)continue;
        const unsigned vendor=test>=7;
        struct cold_model c={.fault=vendor?0:test,.pll=vendor?(test==11?PLL_NEVER:test==13?PLL_DROP:PLL_LOCKS):0};struct model*m=&c.m;
        setup(m,argc==4?UINT64_C(0x3331000021320):UINT64_C(0x3332100021321));
        f=fopen(argv[2],"rb");assert(f);assert(fread(c.rom,1,sizeof(c.rom),f)==sizeof(c.rom));assert(fgetc(f)==EOF);fclose(f);
        m->ccu[0xa88/4]&=~0x80000000u;m->ccu[0xa8c/4]=0x10001;
        m->ccu[0x5c0/4]=0xa5000302;m->ccu[0x5e0/4]=0xb1001003;m->ccu[0x5e4/4]=0x102;
        m->ccu[0x1b10/4]=0x10000;
        if(test==1)c.rom[0x2ca0]^=1;
        if(test==2)m->ccu[0x1998/4]=0;
        if(test==5)m->ve[0xe0/4]=0xdead;
        if(test==6||test==8){m->osc=0x983fb0f7;m->ccu[0]=0xf8675f00;} /* 26MHz DCXO, N=96, M=104. */
        if(vendor) {
            m->ccu[0x220/4]=test==9?0xf9204d00u:test==10?0xf9305a00u:0x41305a00u;
            if(test==9||test==10)c.pll_reads=2; /* Running and locked since an earlier image. */
            m->ccu[0x228/4]=0;m->ccu[0x22c/4]=0x30000000;m->ccu[0x230/4]=0x000b0000;
            m->ccu[0x1918/4]=test==12?0x00030002u:0x00030003u;m->ccu[0x1998/4]=0x00030000;
            m->ccu[0xa88/4]=0;
        }
        struct a733_clock_leases clocks={0};assert(a733_clock_leases_init(&clocks,now,m));
        struct a733_gpu_resource_manager power={0};
        struct a733_gpu_resource_ops po={now,read_any,unused_update,unused_rail};
        assert(a733_gpu_resources_init(&power,&po,m));
        struct a733_gpu_power_resources ready;uint64_t token=0;
        assert(a733_gpu_resources_acquire(&power,2,A733_GPU_RESOURCE_OSC|A733_GPU_RESOURCE_PCK,100000,&token,&ready)==A733_GPU_RESOURCE_OK);
        struct a733_video_platform_config config={.clocks=&clocks,.power=&power,.power_client=2,.owner=99,.handoff=123,
            .pck={token,A733_GPU_RESOURCE_OSC|A733_GPU_RESOURCE_PCK,A733_GPU_RESOURCE_STABLE},
            .module=vendor?0x81000000:0x80000001,.module_hz=vendor?624000000:234000000,.fabric={0,0x80000000,0x80000000},
            .fabric_hz={24000000,24000000,24000000},.start=A733_VIDEO_COLD_NS_ROM};
        a733_clock_mask_t scopes;assert(a733_video_platform_scopes(&config,&scopes));
        const a733_clock_mask_t exclusive=a733_video_platform_exclusive(&config);
        assert(exclusive==(vendor?A733_CLOCK_VE_DEC|A733_CLOCK_PLL_VE0:A733_CLOCK_VE_DEC));
        assert(!(scopes&A733_CLOCK_PLL_VE0)&&(!!(scopes&A733_CLOCK_PLL_VE1))==!vendor);
        if(vendor) {
            /* An inherited start at the same point shares the running PLL. */
            struct a733_video_platform_config inherited=config;inherited.start=A733_VIDEO_INHERITED;
            a733_clock_mask_t shared;assert(a733_video_platform_scopes(&inherited,&shared)&&(shared&A733_CLOCK_PLL_VE0));
            assert(a733_video_platform_exclusive(&inherited)==A733_CLOCK_VE_DEC);
        }
        assert(a733_clock_acquire(&clocks,99,scopes,exclusive,100000,&config.clock)==A733_CLOCK_OK);
        struct a733_clock_token conflict={0};
        assert(a733_clock_acquire(&clocks,100,scopes,A733_CLOCK_VE_DEC,100000,&conflict)!=A733_CLOCK_OK);
        /* Another PLL_VE0 consumer cannot share the PLL being programmed. */
        if(vendor)assert(a733_clock_acquire(&clocks,101,A733_CLOCK_PLL_VE0,A733_CLOCK_NPU,100000,&conflict)==A733_CLOCK_BUSY);
        struct a733_video_platform_ops ops={cold_now,admit,memory,drain,enter,leave,cold_read,cold_codec_write};
        struct a733_video_platform p={0};assert(a733_video_platform_init(&p,&config,&ops,&c));
        struct a733_video_jpeg_job job={bytes,n,0x81000000,65536,0x82000000,2048,0x83000000,1024};
        struct a733_video_jpeg_run run={0};assert(a733_video_jpeg_begin(&run,a733_video_platform_jpeg_ops(),&p,&job,100000));
        int result=0;unsigned polls=0;
        while(!result&&++polls<20000)result=a733_video_jpeg_poll(&run);
        assert(polls<20000&&run.token==42);
        if(vendor) {
            const unsigned table=test==9?0:5;
            if(test==7||test==8||test==9||test==13) {
                assert(result==1&&m->drain==2&&m->trigger&&!m->lock&&c.cold_pulse==2);
                assert(m->events[11].address==CCU+0xa88&&m->events[11].value==0x01000000);
                for(unsigned i=0;i<table;++i)assert(m->events[12+i].address==CCU+0x220);
                assert(m->events[12+table].address==CCU+0xa88&&m->events[12+table].value==0x81000000);
                assert(m->events[13+table].address==VE+0x804&&m->events[14+table].address==VE+0x804);
                assert(c.pll_writes==table&&(m->ccu[0x220/4]==0xf9204d00u));
            }else {
                assert(result==-1&&!m->trigger&&!m->memory&&!m->vereads);
                if(test==10||test==12)assert(!m->writes);
                /* No LOCK: 100 observations, then the unit fails with the PLL
                 * enabled, its output and the module gate closed. */
                if(test==11)assert(m->writes==16&&c.pll_writes==4&&c.pll_reads==101&&
                    m->ccu[0x220/4]==0xe1204d00u&&m->ccu[0xa88/4]==0x01000000);
                unsigned accesses=m->reads+m->writes;
                assert(a733_video_jpeg_poll(&run)==-1&&accesses==m->reads+m->writes);
            }
        }else if(!test||test==6) {
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
            if(test==4)assert(m->writes==1&&p.decoder.unit.locked&&m->lock&&!m->vereads);
            if(test==5)assert(m->writes==14&&m->vereads);
            unsigned accesses=m->reads+m->writes;
            assert(a733_video_jpeg_poll(&run)==-1&&accesses==m->reads+m->writes);
        }
        assert(clocks.slots[0].id==config.clock.id&&power.leases[2].token==token);
        if(vendor)printf("case%u result%d writes%u PLL_writes%u PLL_reads%u PLL_word%08x module%08x drain%u\n",test,result,
            m->writes,c.pll_writes,c.pll_reads,m->ccu[0x220/4],m->ccu[0xa88/4],m->drain);
        else printf("case%u result%d cold_writes%u ROM_reads%u drain%u\n",test,result,test%6?m->writes:14,c.rom_reads,m->drain);
    }
    puts("PASS: immutable-ROM cold admission, reset held through power acknowledgement, preserved counterpart gates, JPEG completion with independent drain, 24/26MHz DCXO references, retained failures. Model only.");
    if(argc==3)puts("PASS: PLL_VE0 at the vendor 624 MHz from the dumped FEL state: enable method 2 in order, three LOCK observations and 20 us before the output gate, adoption of a running vendor PLL, refusal of other running factors and a clear gate control, retained failure without LOCK, a LOCK drop restarting the count, exclusive PLL scope. Model only.");
}
