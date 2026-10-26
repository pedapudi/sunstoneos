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
    /* PLL_VE0 programming: the scenario, whether the entry word needs only
     * the lock-enable toggle, the position in the expected write sequence,
     * reads since lock enable was last set, consecutive LOCK reads, and the
     * times of the last of those reads and of the third LOCK read. */
    unsigned pll,relock,pll_seq,pll_writes,pll_reads,pll_locked,waiting;
    uint64_t pll_lock_at,pll_read_at;
};
#define PLL_LOCKS 1u   /* LOCK appears from the third read after lock enable. */
#define PLL_NEVER 2u   /* LOCK never appears. */
#define PLL_DROP 3u    /* LOCK appears, drops for one read, returns. */
#define PLL_CHANGED 4u /* Another writer changes N during the lock wait. */
#define LOCKBIT (1u<<28)
static int pll_read(struct cold_model *c,uint32_t *out)
{
    struct model *m=&c->m;uint32_t v=m->ccu[0x220/4]&~LOCKBIT;
    if(c->waiting) {
        ++c->pll_reads;
        /* The lock wait reads without the register mutex, each read after
         * its first at least 3 us after the one before. The lock-enable
         * readback precedes the wait, as the kernel's first lock read
         * follows its write at once. */
        if(c->pll_reads>1&&c->pll_locked<3) {
            assert(!m->lock&&(c->pll_reads==2||m->now>=c->pll_read_at+3));
            c->pll_read_at=m->now;
        }
        if(c->pll_reads>2&&c->pll!=PLL_NEVER&&!(c->pll==PLL_DROP&&c->pll_reads==4))v|=LOCKBIT;
        if(c->pll==PLL_CHANGED&&c->pll_reads==4)v^=1u<<8;
        if(c->pll_reads>1&&c->pll_locked<3) {
            c->pll_locked=(v&LOCKBIT)?c->pll_locked+1:0;
            if(c->pll_locked==3)c->pll_lock_at=m->now;
        }
    }else v|=m->ccu[0x220/4]&LOCKBIT;
    *out=v;return 1;
}
/* The expected writes, each a field and its value: enable method 2 from a
 * disabled PLL, or the lock-enable toggle for one left enabled with its
 * output closed; both end with the output gate. A field that already holds
 * its value may be skipped, but never the output gate. */
static void pll_write(struct cold_model *c,uint32_t value)
{
    static const uint32_t enable[5][2]={{1u<<30,1u<<30},{0x0070ff02u,0x00204d00u},{1u<<31,1u<<31},
        {1u<<29,1u<<29},{1u<<27,1u<<27}};
    static const uint32_t relock[3][2]={{1u<<29,0},{1u<<29,1u<<29},{1u<<27,1u<<27}};
    const uint32_t (*seq)[2]=c->relock?relock:enable;const unsigned n=c->relock?3:5;
    struct model *m=&c->m;uint32_t old=m->ccu[0x220/4]&~LOCKBIT;
    assert(c->pll&&c->pck[2]==8&&m->lock&&!(m->ccu[0xa88/4]&0x80000000));
    assert((m->ccu[0xa88/4]&0x0700001f)==0x01000000&&!(value&LOCKBIT));
    while(c->pll_seq<n-1&&(old&seq[c->pll_seq][0])==seq[c->pll_seq][1])++c->pll_seq;
    assert(c->pll_seq<n&&value==((old&~seq[c->pll_seq][0])|seq[c->pll_seq][1])&&value!=old);
    if(c->pll_seq==n-1)assert(c->pll_locked==3&&m->now>=c->pll_lock_at+20);
    c->waiting=(value&0xa0000000u)==0xa0000000u&&c->pll_seq==n-2;
    if(c->waiting)c->pll_reads=c->pll_locked=0;
    m->ccu[0x220/4]=value|(c->pll_seq==n-1?LOCKBIT:0);
    ++c->pll_seq;++c->pll_writes;
}
static int policy_read(void *v,uintptr_t a,uint32_t *out)
{
    struct model *m=&((struct cold_model *)v)->m;
    assert(a>=CCU&&a<CCU+sizeof(m->ccu));*out=m->ccu[(a-CCU)/4];return 1;
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
    if(a==CCU+0x220&&c->pll){++m->reads;return pll_read(c,out);}
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
    /* Every cold-start write passes the native allowlist and stays inside
     * its owned bits. */
    {
        uint32_t owned,current=0;
        assert(a733_video_platform_write_policy(a,width,value,policy_read,c,&owned)==1);
        if(a>=CCU&&a<CCU+sizeof(m->ccu))current=m->ccu[(a-CCU)/4];
        if(a>=CCU&&a<CCU+sizeof(m->ccu)&&a!=CCU+0x220)assert(owned&&!((current^value)&~owned));
        if(a==CCU+0x220)assert(owned==0xf870ff02u&&!((current^value)&~owned));
    }
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
        pll_write(c,value);
    }else {
        assert(a>=CCU&&a<CCU+sizeof(m->ccu)&&c->pck[2]==8);
        uint32_t mask=a==CCU+0xa8c?0x40004:a==CCU+0xa88?(c->pll&&!(m->ccu[0xa88/4]&0x80000000)?0x8700001f:0x80000000):
            a==CCU+0x5c0?1:a==CCU+0x5e0?0x4000:a==CCU+0x5e4?0x40000:0;
        assert(mask&&!((m->ccu[(a-CCU)/4]^value)&~mask));
        if(a==CCU+0xa88&&(value&0x80000000)&&c->pll)assert((m->ccu[0x220/4]&0xf8000000u)==0xf8000000u);
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
/* The native write allowlist against each PLL_VE0 consumer: the encoder at
 * mux 0, the decoder at mux 1 and the NPU at mux 4. */
static void policy(void)
{
    struct cold_model c={0};struct model *m=&c.m;uint32_t owned;
    assert(a733_video_platform_write_policy(CCU+0x220,4,0x41305a00,policy_read,&c,&owned)==1&&owned==0xf870ff02u);
    assert(a733_video_platform_write_policy(CCU+0x220,1,0,policy_read,&c,&owned)==0);
    const struct {uint32_t module,word;int allowed;} rows[]={
        {0xa88,0x81000000,0},{0xa88,0x80000000,1},{0xa80,0x80000000,0},{0xa80,0x82000000,1},
        {0xb00,0x84000000,0},{0xb00,0x80000000,1},{0xa80,0x00000000,1},{0xb00,0x04000000,1}};
    for(unsigned i=0;i<sizeof(rows)/sizeof(rows[0]);++i) {
        m->ccu[0xa80/4]=m->ccu[0xa88/4]=m->ccu[0xb00/4]=0;m->ccu[rows[i].module/4]=rows[i].word;
        assert(a733_video_platform_write_policy(CCU+0x220,4,0x41305a00,policy_read,&c,&owned)==rows[i].allowed);
    }
    m->ccu[0xa80/4]=m->ccu[0xb00/4]=0;
    m->ccu[0xa88/4]=0x80000000;
    assert(a733_video_platform_write_policy(CCU+0xa88,4,0x01000000,policy_read,&c,&owned)==0);
    assert(a733_video_platform_write_policy(CCU+0xa88,4,0x81000000,policy_read,&c,&owned)==1&&owned==0x80000000u);
    m->ccu[0xa88/4]=0;
    assert(a733_video_platform_write_policy(CCU+0xa88,4,0x01000000,policy_read,&c,&owned)==1&&owned==0x0700001fu);
    assert(a733_video_platform_write_policy(CCU+0x240,4,0,policy_read,&c,&owned)==0);
}
int main(int argc,char **argv)
{
    assert(argc==3||(argc==4&&!strcmp(argv[3],"--reset")));uint8_t bytes[65536];
    FILE*f=fopen(argv[1],"rb");assert(f);size_t n=fread(bytes,1,sizeof(bytes),f);assert(n&&n<sizeof(bytes));fclose(f);
    /* Cases 7-21 start at the vendor operating point with PLL_VE0 as the
     * port 3-3 clock dump read it at FEL entry (board-session-20261025-24,
     * clock-audit-b): disabled at 0x41305a00, gate controls 0x00030003 and
     * status 0x00030000, module word 0. Each row gives the entry word, the
     * scenario, whether the start completes, the PLL writes and the final
     * PLL word. */
    static const struct {uint32_t entry;unsigned scenario,ok,writes;uint32_t final;const char *name;} vendor_cases[]={
        {0x41305a00u,PLL_LOCKS,1,4,0xf9204d00u,"dumped entry word, LDO already on"},
        {0x41305a00u,PLL_LOCKS,1,4,0xf9204d00u,"26 MHz board"},
        {0xf9204d00u,PLL_LOCKS,1,0,0xf9204d00u,"running vendor profile adopted"},
        {0xf9305a00u,PLL_LOCKS,0,0,0xf9305a00u,"running at other factors refused"},
        {0x41305a00u,PLL_NEVER,0,3,0xe1204d00u,"no LOCK: 100 reads, output closed"},
        {0x41305a00u,PLL_LOCKS,0,0,0x41305a00u,"gate control clear refused"},
        {0x41305a00u,PLL_DROP,1,4,0xf9204d00u,"LOCK drops once"},
        {0xc1305a00u,PLL_LOCKS,0,0,0xc1305a00u,"entry with enable set refused"},
        {0x61305a00u,PLL_LOCKS,0,0,0x61305a00u,"entry with lock enable set refused"},
        {0x41305a20u,PLL_LOCKS,0,0,0x41305a20u,"entry with a lock level set refused"},
        {0x40305a00u,PLL_LOCKS,1,4,0xf8204d00u,"bit 24 clear stays clear"},
        {0x01305a00u,PLL_LOCKS,1,5,0xf9204d00u,"LDO off is written"},
        {0x41305a00u,PLL_CHANGED,0,3,0xe1204d00u,"a changed word during the wait"},
        {0xe1204d00u,PLL_LOCKS,1,3,0xf9204d00u,"left enabled by a failed start: relocked"},
        {0xf1204d00u,PLL_LOCKS,1,3,0xf9204d00u,"left enabled and locked: relocked"},
    };
    const unsigned cases=7+sizeof(vendor_cases)/sizeof(vendor_cases[0]);
    for(unsigned test=0;test<cases;++test) {
        if(argc==4&&test)continue;
        const unsigned vendor=test>=7,row=vendor?test-7:0;
        struct cold_model c={.fault=vendor?0:test,.pll=vendor?vendor_cases[row].scenario:0,
            .relock=vendor&&(vendor_cases[row].entry&0xa0000000u)==0xa0000000u};struct model*m=&c.m;
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
            m->ccu[0x220/4]=vendor_cases[row].entry;
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
            const unsigned w=vendor_cases[row].writes;
            assert(c.pll_writes==w&&m->ccu[0x220/4]==vendor_cases[row].final);
            if(vendor_cases[row].ok) {
                assert(result==1&&m->drain==2&&m->trigger&&!m->lock&&c.cold_pulse==2);
                assert(m->events[11].address==CCU+0xa88&&m->events[11].value==0x01000000);
                for(unsigned i=0;i<w;++i)assert(m->events[12+i].address==CCU+0x220);
                assert(m->events[12+w].address==CCU+0xa88&&m->events[12+w].value==0x81000000);
                assert(m->events[13+w].address==VE+0x804&&m->events[14+w].address==VE+0x804);
            }else {
                assert(result==-1&&!m->trigger&&!m->memory&&!m->vereads);
                /* A refused entry writes nothing. A failed wait leaves the
                 * PLL enabled with its output and the module gate closed. */
                if(!w)assert(!m->writes);
                else assert(m->writes==12+w&&!(m->ccu[0xa88/4]&0x80000000));
                if(c.pll==PLL_NEVER)assert(c.pll_reads==101);
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
        if(vendor)printf("case%u %s: result%d writes%u PLL_writes%u PLL_reads%u PLL_word%08x module%08x\n",test,
            vendor_cases[row].name,result,m->writes,c.pll_writes,c.pll_reads,m->ccu[0x220/4],m->ccu[0xa88/4]);
        else printf("case%u result%d cold_writes%u ROM_reads%u drain%u\n",test,result,test%6?m->writes:14,c.rom_reads,m->drain);
    }
    puts("PASS: immutable-ROM cold admission, reset held through power acknowledgement, preserved counterpart gates, JPEG completion with independent drain, 24/26MHz DCXO references, retained failures. Model only.");
    if(argc==3){policy();puts("PASS: PLL_VE0 at the vendor 624 MHz from the dumped FEL state and fourteen other entry and lock cases; the lock wait without the register mutex; the native write allowlist on every cold-start write and against every PLL_VE0 consumer; exclusive PLL scope. Model only.");}
}
