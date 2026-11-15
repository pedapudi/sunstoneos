/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The decoder's cold start, composed with the actual power-domain owner.
 *
 * The JPEG platform model (a733-video-platform.c, whose own main is not
 * run) supplies the CCU, GIC and decoder words and the JPEG job callbacks.
 * checks/power-domains-model.h supplies the actual PMIC provider, rail ledger
 * and power-domain owner over modeled PPU words: PCK domain 2 lives only in
 * d.ppu[2], which the owner writes and the decoder's baseline reads. The
 * decoder's row request is answered by service steps run between JPEG polls.
 * The binding has its own microsecond clock (the JPEG model's); the owner
 * runs on the PMIC model clock. */
#pragma GCC diagnostic push
/* Renaming the unused C main removes its implicit return0 rule. */
#pragma GCC diagnostic ignored "-Wreturn-type"
#define main inherited_main
#include "a733-video-platform.c"
#undef main
#pragma GCC diagnostic pop
#include "power-domains-model.h"
static struct pmic_model *const pm=&m;
#define D2_POLICY (A733_POWER_PCK_DOMAIN(2))
struct cold_model {
    struct model m;
    uint8_t rom[45056];
    unsigned fault,rom_reads,cold_pulse;
    /* PLL_VE0 programming: the scenario, whether the entry word needs only
     * the lock-enable toggle, the position in the expected write sequence,
     * reads since lock enable was last set, consecutive LOCK reads, and the
     * times of the last of those reads and of the third LOCK read. */
    unsigned pll,relock,pll_seq,pll_writes,pll_reads,pll_locked,waiting,settle_seen,stopping;
    uint64_t pll_lock_at,pll_read_at;
    /* The row, its rail token and its wakes; the critical gates read since
     * the 0x5e4 write; 0x5e4 key bits 1, 8 and 9 reading zero; PCK reads by
     * the decoder; accesses per register-lock hold. */
    struct a733_power_domain_row row;struct a733_rail_token token;
    unsigned wakes,gate_reads,key_reads_zero,pck_reads,quiesced;
    unsigned hold_start,holds,hold_accesses[8],hold_writes[8],pll_policy_writes,writes_at_hold[8];
};
static unsigned domain_on(void){return (d.ppu[2][0]&15)==8&&(d.ppu[2][2]&15)==8;}
static unsigned row_on(struct cold_model *c){return a733_power_domain_state(&c->row)==A733_POWER_ROW_ON;}
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
         * its first at least 3 us after the one before. The model's clock is
         * the binding's whole-microsecond counter: a reading t is some
         * instant in [t, t+1), so 3 us are guaranteed only 4 readings apart,
         * and 20 us only 21 apart. The lock-enable readback precedes the
         * wait, as the kernel's first lock read follows its write at once. */
        if(c->pll_reads>1&&c->pll_locked<3) {
            assert(!m->lock&&(c->pll_reads==2||m->now>=c->pll_read_at+4));
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
 * output closed; both end with the output gate. At quiesce, the kernel's
 * disable order. A field that already holds its value may be skipped, but
 * never the output gate. */
static void pll_write(struct cold_model *c,uint32_t value)
{
    static const uint32_t enable[5][2]={{1u<<30,1u<<30},{0x0070ff02u,0x00204d00u},{1u<<31,1u<<31},
        {1u<<29,1u<<29},{1u<<27,1u<<27}};
    static const uint32_t relock[3][2]={{1u<<29,0},{1u<<29,1u<<29},{1u<<27,1u<<27}};
    static const uint32_t stop[4][2]={{1u<<27,0},{1u<<29,0},{1u<<31,0},{1u<<30,0}};
    const uint32_t (*seq)[2]=c->stopping?stop:c->relock?relock:enable;
    const unsigned n=c->stopping?4:c->relock?3:5;
    struct model *m=&c->m;uint32_t old=m->ccu[0x220/4]&~LOCKBIT;
    assert(c->pll&&domain_on()&&m->lock&&!(m->ccu[0xa88/4]&0x80000000));
    assert((m->ccu[0xa88/4]&0x0700001f)==0x01000000&&!(value&LOCKBIT));
    while(c->pll_seq<n-1&&(old&seq[c->pll_seq][0])==seq[c->pll_seq][1])++c->pll_seq;
    assert(c->pll_seq<n&&value==((old&~seq[c->pll_seq][0])|seq[c->pll_seq][1])&&value!=old);
    if(!c->stopping&&c->pll_seq==n-1)assert(c->pll_locked==3&&m->now>=c->pll_lock_at+21);
    c->waiting=!c->stopping&&(value&0xa0000000u)==0xa0000000u&&c->pll_seq==n-2;
    if(c->waiting)c->pll_reads=c->pll_locked=0;
    m->ccu[0x220/4]=value|(!c->stopping&&c->pll_seq==n-1?LOCKBIT:0);
    ++c->pll_seq;++c->pll_writes;
}
static int policy_read(void *v,uintptr_t a,uint32_t *out)
{
    struct model *m=&((struct cold_model *)v)->m;
    assert(a>=CCU&&a<CCU+sizeof(m->ccu));*out=m->ccu[(a-CCU)/4];return 1;
}
/* In the PLL cases the binding's clock advances 1 us per 64 calls, so that
 * only its explicit waits, and not the time checks around each access, space
 * the LOCK reads and the settle, and a reading taken just after an access
 * usually equals the reading during it. */
static int cold_now(void *v,uint64_t *n)
{
    struct cold_model *c=v;
    if(!c->pll)return now(v,n);
    static unsigned calls;
    if(++calls%64==0)++c->m.now;
    *n=c->m.now;return 1;
}
/* The first register-lock entry after the third LOCK read is the open step,
 * which follows the end of the lock wait: the settle must already hold. */
static int cold_enter(void *v,uint64_t dl)
{
    struct cold_model *c=v;
    if(c->pll&&c->pll_locked==3&&!c->settle_seen){assert(c->m.now>=c->pll_lock_at+21);c->settle_seen=1;}
    int r=enter(v,dl);
    c->hold_start=c->m.reads+c->m.writes+c->rom_reads;
    if(c->holds<8)c->writes_at_hold[c->holds]=c->m.writes;
    return r;
}
static int cold_leave(void *v)
{
    struct cold_model *c=v;
    if(c->holds<8) {
        c->hold_accesses[c->holds]=c->m.reads+c->m.writes+c->rom_reads-c->hold_start;
        c->hold_writes[c->holds]=c->m.writes-c->writes_at_hold[c->holds];
    }
    ++c->holds;
    return leave(v);
}
static const uint32_t gate_bits[3][2]={{0x5c0,1u<<0},{0x5e0,1u<<14},{0x5e4,1u<<18}};
static int cold_read(void *v,uintptr_t a,uint32_t *out)
{
    struct cold_model *c=v;struct model *m=&c->m;
    if(a>=0x17000&&a<0x22000) {
        assert(m->lock&&!m->writes);++c->rom_reads;
        memcpy(out,c->rom+(a-0x17000),4);return 1;
    }
    if(a==CCU+0x220&&c->pll){++m->reads;return pll_read(c,out);}
    /* The decoder reads PCK domain 2 only in its baseline, after the
     * owner's ON answer, and never the delay row. */
    if(a>=PCK&&a<PCK+0x1000) {
        assert(m->lock&&row_on(c)&&(a==PCK||a==PCK+8));++m->reads;++c->pck_reads;
        *out=d.ppu[2][(a-PCK)/4];return 1;
    }
    for(unsigned i=0;i<3;++i)if(a==CCU+gate_bits[i][0]&&row_on(c)&&(m->ccu[0x5e4/4]&(1u<<18)))c->gate_reads|=1u<<i;
    if(a==CCU+0x5e4&&c->key_reads_zero){assert(m->lock);++m->reads;*out=m->ccu[0x5e4/4]&~0x302u;return 1;}
    if(a>=VE&&a<VE+sizeof(m->ve)) {
        assert(c->rom_reads==15&&domain_on());
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
    if(m->memory&&!c->quiesced&&!(a>=CCU&&a<CCU+0x2000))return write_reg(v,a,width,value);
    /* Every cold-start and quiesce write passes the native allowlist and
     * stays inside its owned bits. */
    {
        uint32_t owned,cur=0;
        assert(a733_video_platform_write_policy(a,width,value,policy_read,c,&owned)==1);
        if(a>=CCU&&a<CCU+sizeof(m->ccu))cur=m->ccu[(a-CCU)/4];
        if(a>=CCU&&a<CCU+sizeof(m->ccu)&&a!=CCU+0x220)assert(owned&&!((cur^value)&~owned));
        if(a==CCU+0x220){assert(owned==0xf870ff02u&&!((cur^value)&~owned));++c->pll_policy_writes;}
    }
    assert(m->writes<800);
    m->events[m->writes++]=(struct event){a,width,value};
    if(c->fault==4)return -1;
    /* The decoder writes after the owner's ON answer only, and never a PCK
     * register or the AHB and MBUS master gates. */
    assert(row_on(c)&&domain_on()&&!(a>=PCK&&a<PCK+0x1000)&&a!=CCU+0x5c0&&a!=CCU+0x5e0);
    if(a==VE+0x804) {
        assert(m->ccu[0xa88/4]&0x80000000);
        assert(!m->memory&&c->cold_pulse<2);
        assert((value&0x11)==(c->cold_pulse?0:0x11));
        assert(!((value^m->ve[0x804/4])&~0x11));
        m->ve[0x804/4]=value;++c->cold_pulse;
    }else if(a==CCU+0x220) {
        pll_write(c,value);
    }else {
        assert(a>=CCU&&a<CCU+sizeof(m->ccu));
        const uint32_t cur=m->ccu[(a-CCU)/4];
        uint32_t mask=a==CCU+0xa8c?0x40004:a==CCU+0xa88?(c->pll&&!(cur&0x80000000)?0x8700001f:0x80000000):
            a==CCU+0x5e4?0x40302:0;
        assert(mask&&!((cur^value)&~mask));
        /* The MBUS clock gate opens with the whole key value OR'd in, and
         * before any reset, bus-gate or clock write (A1 follows it). */
        if(a==CCU+0x5e4)assert(value==(cur|0x40302)&&!(cur&(1u<<18))&&!c->gate_reads);
        /* A1: reset is released only after the three critical gates were
         * read, after the 0x5e4 write, and all read set. */
        if(a==CCU+0xa8c&&(value&(1u<<18))&&!(cur&(1u<<18))) {
            assert(c->gate_reads==7);
            for(unsigned i=0;i<3;++i)assert(m->ccu[gate_bits[i][0]/4]&gate_bits[i][1]);
        }
        if(a==CCU+0xa88&&(value&0x80000000)&&c->pll)assert((m->ccu[0x220/4]&0xf8000000u)==0xf8000000u);
        m->ccu[(a-CCU)/4]=value;
    }
    return 1;
}
static int cold_codec_write(void*v,uintptr_t a,unsigned width,uint32_t value)
{
    struct cold_model*c=v;
    return c->m.memory&&!c->quiesced&&!(a>=CCU&&a<CCU+0x2000)?write_reg(v,a,width,value):cold_write(v,a,width,value);
}
static void row_wake(void *v){struct cold_model *c=v;++c->wakes;}
/* The native write allowlist: each PLL_VE0 consumer, the module gate, the
 * MBUS clock key, and the critical gates and PCK600 words it refuses. */
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
    /* With the module gate open, only bit 31 may change: closing it is
     * admitted, a selector change with it is not. */
    m->ccu[0xa88/4]=0x81000000;
    assert(a733_video_platform_write_policy(CCU+0xa88,4,0x01000000,policy_read,&c,&owned)==1&&owned==0x80000000u);
    assert(a733_video_platform_write_policy(CCU+0xa88,4,0x00000000,policy_read,&c,&owned)==0);
    assert(a733_video_platform_write_policy(CCU+0xa88,1,0x01000000,policy_read,&c,&owned)==0);
    assert(a733_video_platform_write_policy(CCU+0xa88,4,0x81000000,policy_read,&c,&owned)==1&&owned==0x80000000u);
    m->ccu[0xa88/4]=0;
    assert(a733_video_platform_write_policy(CCU+0xa88,4,0x01000000,policy_read,&c,&owned)==1&&owned==0x0700001fu);
    assert(a733_video_platform_write_policy(CCU+0x240,4,0,policy_read,&c,&owned)==0);
    /* 0x5e4: only a word carrying the whole key value; no critical-gate,
     * PCK600 or delay-row write. */
    assert(a733_video_platform_write_policy(CCU+0x5e4,4,0x40306,policy_read,&c,&owned)==1&&owned==0x40302u);
    assert(a733_video_platform_write_policy(CCU+0x5e4,4,0x40004,policy_read,&c,&owned)==0);
    assert(a733_video_platform_write_policy(CCU+0x5e4,4,0x00306,policy_read,&c,&owned)==0);
    const uint32_t refused[][2]={{CCU+0x5c0,0x110103ff},{CCU+0x5e0,0xf1055803},{PCK,0xa5a50008},{PCK+0x170,0x1f1f1f},
        {PCK+0x174,0x1f1f},{PCK+0xc00,0x08080808},{PCK+0xc04,0x0808},{PCK+0xc10,8}};
    for(unsigned i=0;i<sizeof(refused)/sizeof(refused[0]);++i)
        assert(a733_video_platform_write_policy(refused[i][0],4,refused[i][1],policy_read,&c,&owned)==0);
}
/* The port 3-3 FEL dump (board-session-20261025-24, clock-audit-b): the
 * three gate words, and PLL_VE0 disabled at 0x41305a00 with gate controls
 * 0x00030003 and status 0x00030000, module word 0. */
#define FEL_5C0 UINT32_C(0x110103ff)
#define FEL_5E0 UINT32_C(0xf1055803)
#define FEL_5E4 UINT32_C(0x4)
#define FEL_PLL UINT32_C(0x41305a00)
struct lifetime {
    /* Domain-2 policy and status nibbles at entry, the PPU acknowledgement
     * delay, a stalled PPU; the model fault; the PLL scenario (0: a
     * non-vendor point with nothing programmed) and PLL entry word; 0x5e4 key
     * bits reading zero; an edit of the entry words. */
    uint32_t policy,status;unsigned ack_us,stall,fault,pll;uint32_t pll_entry;unsigned key_zero;
    void (*edit)(struct model *);
    unsigned service; /* the OS service's open (apps/os/video_decode.c) instead of the JPEG job */
};
struct outcome {
    int result;unsigned writes,owner_ppu_writes,owner_commands,decoder_pck_reads,polls_after_on;
    enum a733_power_row_state row;struct a733_video_cold_record rec;
};
static uint8_t jpeg_bytes[65536];static size_t jpeg_n;static uint8_t rom_bytes[45056];static uint64_t version;
/* The entry words: the platform model's, with the decoder in reset and
 * gated, the FEL gate words and autonomous power at its entry value; at the
 * vendor point also the FEL PLL_VE0 words. */
static void fel_words(struct cold_model *c,const struct lifetime *l)
{
    struct model *m=&c->m;
    /* FEL (port 3-3) and OS entry (port 3-1) read 0xa8c = 0 and 0x1b10 = 0x20000 with the module gate closed; the
     * model keeps 0x1b10 constant, as no reading shows it move. */
    m->ccu[0xa88/4]&=~0x80000000u;m->ccu[0xa8c/4]=0;
    m->ccu[0x5c0/4]=FEL_5C0;m->ccu[0x5e0/4]=FEL_5E0;m->ccu[0x5e4/4]=FEL_5E4;m->ccu[0x1b10/4]=0x20000;
    if(l->pll) {
        m->ccu[0x220/4]=l->pll_entry;m->ccu[0x228/4]=0;m->ccu[0x22c/4]=0x30000000;m->ccu[0x230/4]=0x000b0000;
        m->ccu[0x1918/4]=0x00030003u;m->ccu[0x1998/4]=0x00030000;m->ccu[0xa88/4]=0;
    }
}
/* One power lifetime: a fresh owner and decoder. ccu_in, when given,
 * replaces the CCU words (a later lifetime); ppu_in the domain-2 nibbles.
 * Runs the JPEG job and, when it completes and quiesce is set, the quiesce. */
/* The OS service's open (apps/os/video_decode.c, do_open): acquire with an empty job until started, the three
 * identity words through the codec read call, then prepare's identity admission, which returns 0 with the version
 * set and the register lock released. One deadline for every call. */
static const struct a733_video_jpeg_job no_job;
static int service_open(struct a733_video_platform *p,uint64_t d)
{
    const struct a733_video_jpeg_ops *o=a733_video_platform_jpeg_ops();uint64_t t=0;uint32_t w[3];
    const int r=o->acquire(p,&no_job,d,&t);
    if(r!=1)return r;
    assert(o->read(p,t,0xf0,&w[0],d)==1&&o->read(p,t,0xe0,&w[1],d)==1&&o->read(p,t,0xe4,&w[2],d)==1);
    assert(((uint64_t)(w[1]&0xfffff)<<32|(w[2]&0xfffff))==version);
    assert(o->prepare(p,t,&no_job,NULL,0,0,d)==0&&p->decoder.version==version&&!p->decoder.unit.locked);
    return 1;
}
static struct outcome lifetime(struct cold_model *c,const struct lifetime *l,const uint32_t *ccu_in,
    const uint32_t *ppu_in,unsigned quiesce)
{
    struct model *m=&c->m;
    domains_model_setup(0x28,23);
    const uint32_t pol=ppu_in?ppu_in[0]:l->policy,sta=ppu_in?ppu_in[1]:l->status;
    d.ppu[2][0]|=pol;d.ppu[2][2]|=sta;d.ack_us=l->ack_us;d.stall[2]=l->stall;
    memset(c,0,sizeof(*c));
    c->fault=l->fault;c->pll=l->pll;c->key_reads_zero=l->key_zero;
    setup(m,version);memcpy(c->rom,rom_bytes,sizeof(c->rom));
    fel_words(c,l);
    if(ccu_in)memcpy(m->ccu,ccu_in,sizeof(m->ccu));
    if(l->edit)l->edit(m);
    c->relock=c->pll&&(m->ccu[0x220/4]&0x80000000u);
    const unsigned vendor=c->pll!=0;
    struct a733_clock_leases clocks={0};assert(a733_clock_leases_init(&clocks,now,m));
    struct a733_gpu_resource_manager power={0};
    struct a733_gpu_resource_ops po={now,read_any,unused_update,unused_rail};
    assert(a733_gpu_resources_init(&power,&po,m));
    struct a733_gpu_power_resources ready;uint64_t token=0;
    assert(a733_gpu_resources_acquire(&power,2,A733_GPU_RESOURCE_OSC|A733_GPU_RESOURCE_PCK,100000,&token,&ready)==A733_GPU_RESOURCE_OK);
    assert(a733_power_domain_row_init(&c->row,&d.owner,2,90,&c->token));
    c->row.wake=row_wake;c->row.wake_context=c;
    domains_boot();c->token=domains_rail(90,A733_RAIL_DCDC2);
    assert(a733_power_authority_load(&pm->rails.authority)&A733_POWER_DCDC2_VERIFIED);
    struct a733_video_platform_config config={.clocks=&clocks,.power=&power,.power_client=2,.owner=99,.handoff=123,
        .pck={token,A733_GPU_RESOURCE_OSC|A733_GPU_RESOURCE_PCK,A733_GPU_RESOURCE_STABLE},
        .module=vendor?0x81000000:0x80000001,.module_hz=vendor?624000000:234000000,.fabric={0,0x80000000,0x80000000},
        .fabric_hz={24000000,24000000,24000000},.start=A733_VIDEO_COLD_NS_ROM,.row=&c->row};
    a733_clock_mask_t scopes;assert(a733_video_platform_scopes(&config,&scopes));
    const a733_clock_mask_t exclusive=a733_video_platform_exclusive(&config);
    assert(exclusive==(vendor?A733_CLOCK_VE_DEC|A733_CLOCK_PLL_VE0:A733_CLOCK_VE_DEC));
    assert(!(scopes&A733_CLOCK_PLL_VE0)&&(!!(scopes&A733_CLOCK_PLL_VE1))==!vendor);
    assert(a733_clock_acquire(&clocks,99,scopes,exclusive,100000,&config.clock)==A733_CLOCK_OK);
    /* Another PLL_VE0 consumer cannot share the PLL being programmed. */
    struct a733_clock_token conflict={0};
    if(vendor)assert(a733_clock_acquire(&clocks,101,A733_CLOCK_PLL_VE0,A733_CLOCK_NPU,100000,&conflict)==A733_CLOCK_BUSY);
    struct a733_video_platform_ops ops={cold_now,admit,memory,drain,cold_enter,cold_leave,cold_read,cold_codec_write};
    /* A cold start needs a domain-2 row. */
    {
        struct a733_video_platform_config bad=config;struct a733_video_platform q={0};
        bad.row=0;assert(!a733_video_platform_init(&q,&bad,&ops,c));
    }
    struct a733_video_platform p={0};assert(a733_video_platform_init(&p,&config,&ops,c));
    struct a733_video_jpeg_job job={jpeg_bytes,jpeg_n,0x81000000,65536,0x82000000,2048,0x83000000,1024};
    struct a733_video_jpeg_run run={0};assert(a733_video_jpeg_begin(&run,a733_video_platform_jpeg_ops(),&p,&job,100000));
    const unsigned ppu_writes=d.ppu_writes;
    int result=0;unsigned polls=0,on_poll=0,checked_poll=0;
    while(!result&&++polls<20000) {
        result=l->service?service_open(&p,run.deadline):a733_video_jpeg_poll(&run);
        if(!checked_poll&&p.decoder.checked)checked_poll=polls;
        /* The service answers the row; the decoder's owner task blocks on
         * the row's wake meanwhile. */
        if(!result&&a733_power_domain_state(&c->row)==A733_POWER_ROW_PENDING) {
            if(d.notified||pm->now>=d.wake_at)service_step();
            else if(d.wake_at!=UINT64_MAX)pm->now=d.wake_at;
            if(a733_power_domain_state(&c->row)!=A733_POWER_ROW_PENDING)on_poll=polls;
        }
    }
    assert(polls<20000);
    struct outcome o={.result=result,.writes=m->writes,.owner_ppu_writes=d.ppu_writes-ppu_writes,
        .owner_commands=d.owner.transition[3].commands,.decoder_pck_reads=c->pck_reads,
        .polls_after_on=checked_poll&&on_poll?checked_poll-on_poll:0,.row=a733_power_domain_state(&c->row),
        .rec=p.decoder.cold};
    /* The decoder's row is answered ON before its first write. Adopted ON,
     * the first poll's request is answered at once. */
    if(checked_poll&&on_poll)assert(o.polls_after_on==1);
    if(result==1&&quiesce) {
        const unsigned before=m->writes;c->quiesced=1;c->stopping=1;c->pll_seq=0;
        assert(a733_video_decoder_quiesce(&p.decoder,run.deadline+200000)==1);
        assert(!m->lock&&m->writes>before);
        /* Closed for the rest of the power lifetime: a later open is refused with no access. */
        {
            const unsigned accesses=m->reads+m->writes;uint64_t t=0;
            assert(a733_video_platform_jpeg_ops()->acquire(&p,&no_job,run.deadline+200000,&t)==-1&&
                m->reads+m->writes==accesses&&!p.decoder.unit.failed&&!m->lock);
        }
        /* The quiesce leaves the entry state and domain 2 ON. */
        assert(!(m->ccu[0xa88/4]&0x80000000)&&!(m->ccu[0xa8c/4]&0x40004)&&domain_on());
        if(vendor)assert(m->ccu[0x220/4]==((A733_VIDEO_PLL_FACTORS)|(m->ccu[0x220/4]&(1u<<24))));
    }
    assert(clocks.slots[0].id==config.clock.id&&power.leases[2].token==token);
    return o;
}
static void print_holds(const char *name,const struct cold_model *c)
{
    printf("holds %s:",name);
    for(unsigned i=0;i<c->holds&&i<8;++i)printf(" %u/%u",c->hold_accesses[i],c->hold_writes[i]);
    printf(" (accesses/writes per register-lock hold) pll_policy_writes%u\n",c->pll_policy_writes);
}
static void rom_flip(struct model *m){((struct cold_model *)(void *)m)->rom[0x2ca0]^=1;} /* m is the first member */
static void peri0_clear(struct model *m){m->ccu[0x1998/4]=0;}
static void mhz26(struct model *m){m->osc=0x983fb0f7;m->ccu[0]=0xf8675f00;} /* 26 MHz DCXO, N=96, M=104. */
static void version_bad(struct model *m){m->ve[0xe0/4]=0xdead;}
static void gate_control_clear(struct model *m){m->ccu[0x1918/4]=0x00030002u;}
static void reset_released(struct model *m){m->ccu[0xa8c/4]|=1u<<18;}
static void mbus_clear(struct model *m){m->ccu[0x5e0/4]&=~(1u<<14);}
static void ahb_clear(struct model *m){m->ccu[0x5c0/4]&=~1u;}
static void mclk_open(struct model *m){m->ccu[0x5e4/4]=0x40306;}
int main(int argc,char **argv)
{
    assert(argc==3||(argc==4&&!strcmp(argv[3],"--reset")));
    FILE*f=fopen(argv[1],"rb");assert(f);jpeg_n=fread(jpeg_bytes,1,sizeof(jpeg_bytes),f);assert(jpeg_n&&jpeg_n<sizeof(jpeg_bytes));fclose(f);
    f=fopen(argv[2],"rb");assert(f);assert(fread(rom_bytes,1,sizeof(rom_bytes),f)==sizeof(rom_bytes));assert(fgetc(f)==EOF);fclose(f);
    version=argc==4?UINT64_C(0x3331000021320):UINT64_C(0x3332100021321);
    static struct cold_model c;struct model *const vm=&c.m;
    /* Cases 0-6 at a non-vendor point (PLL_VE1 at 234 MHz, nothing
     * programmed), domain 0/0 and the FEL gate words: 0 nominal, 1 a ROM byte
     * changed, 2 PERI0 status clear, 3 the PPU never acknowledges, 4 an
     * uncertain write, 5 an unlisted VE version, 6 the 26 MHz board. */
    static void (*const edits[7])(struct model *)={0,rom_flip,peri0_clear,0,0,version_bad,mhz26};
    for(unsigned test=0;test<7;++test) {
        if(argc==4&&test)continue;
        const struct lifetime l={.ack_us=10,.stall=test==3,.fault=test==4?4:0,.edit=edits[test]};
        const struct outcome o=lifetime(&c,&l,0,0,0);
        if(!test||test==6) {
            /* ON from 0/0 by the owner (delay row and one command), then one
             * open of 0x5e4, reset, bus gate, module gate and the VCU pulse. */
            assert(o.result==1&&vm->drain==2&&vm->trigger&&!vm->lock&&c.cold_pulse==2);
            assert(o.owner_ppu_writes==6&&o.owner_commands==1&&o.decoder_pck_reads==2);
            assert(vm->events[0].address==CCU+0x5e4&&vm->events[0].value==0x40306);
            assert(vm->events[1].address==CCU+0xa8c&&vm->events[1].value==0x40000);
            assert(vm->events[2].address==CCU+0xa8c&&vm->events[2].value==0x40004);
            assert(vm->events[3].address==CCU+0xa88&&vm->events[3].value==0x80000001);
            assert(vm->events[4].address==VE+0x804&&vm->events[4].value==0x11);
            assert(vm->events[5].address==VE+0x804&&vm->events[5].value==0);
            if(argc==4)assert(vm->events[7].address==VE+4&&vm->events[8].address==VE+4);
            assert(vm->ccu[0x5c0/4]==FEL_5C0&&vm->ccu[0x5e0/4]==FEL_5E0&&vm->ccu[0x5e4/4]==0x40306);
            assert(o.rec.entry_gates[0]==FEL_5C0&&o.rec.entry_gates[1]==FEL_5E0&&o.rec.entry_gates[2]==FEL_5E4);
            assert(o.rec.gates[2]==0x40306&&o.rec.mclk_written==0x40306&&o.rec.mclk_readback==0x40306);
            assert(o.rec.entry_at<=o.rec.entered_at&&o.rec.entered_at<=o.rec.on_at&&o.rec.on_at<=o.rec.hold_a_at&&
                o.rec.hold_a_at<=o.rec.locked_at&&o.rec.locked_at<=o.rec.hold_b_at);
            if(!test)print_holds("non-vendor",&c);
        }else {
            assert(o.result==-1&&!vm->trigger&&!vm->memory);
            if(test==1||test==2)assert(!vm->writes&&!vm->vereads&&!o.owner_ppu_writes&&o.row==A733_POWER_ROW_IDLE);
            /* The domain never acknowledges: the owner fails the row at its
             * bound and the decoder has written nothing. */
            if(test==3)assert(!vm->writes&&o.owner_commands==1&&o.row==A733_POWER_ROW_FAILED&&!vm->lock);
            if(test==4)assert(vm->writes==1&&vm->lock);
            if(test==5)assert(vm->writes==6&&vm->vereads&&!vm->lock);
        }
        printf("case%u result%d writes%u owner_ppu_writes%u\n",test,o.result,o.writes,o.owner_ppu_writes);
    }
    if(argc==4){puts("PASS: local-reset decoder version cold start");return 0;}
    /* Cases 7-22 at the vendor operating point from the FEL words, domain
     * 0/0. Each row gives the PLL entry word, the scenario, whether the start
     * completes, the PLL writes and the final PLL word. */
    static const struct {uint32_t entry;unsigned scenario,ok,writes;uint32_t final;void (*edit)(struct model *);const char *name;}
    vendor_cases[]={
        {FEL_PLL,PLL_LOCKS,1,4,0xf9204d00u,0,"dumped entry word, LDO already on"},
        {FEL_PLL,PLL_LOCKS,1,4,0xf9204d00u,mhz26,"26 MHz board"},
        {0xf9204d00u,PLL_LOCKS,1,0,0xf9204d00u,0,"running vendor profile adopted"},
        {0xf9305a00u,PLL_LOCKS,0,0,0xf9305a00u,0,"running at other factors refused"},
        {FEL_PLL,PLL_NEVER,0,3,0xe1204d00u,0,"no LOCK: 100 reads, output closed"},
        {FEL_PLL,PLL_LOCKS,0,0,FEL_PLL,gate_control_clear,"gate control clear refused"},
        {FEL_PLL,PLL_DROP,1,4,0xf9204d00u,0,"LOCK drops once"},
        {0xc1305a00u,PLL_LOCKS,0,0,0xc1305a00u,0,"entry with enable set refused"},
        {0x61305a00u,PLL_LOCKS,0,0,0x61305a00u,0,"entry with lock enable set refused"},
        {0x41305a20u,PLL_LOCKS,0,0,0x41305a20u,0,"entry with a lock level set refused"},
        {0x40305a00u,PLL_LOCKS,1,4,0xf8204d00u,0,"bit 24 clear stays clear"},
        {0x01305a00u,PLL_LOCKS,1,5,0xf9204d00u,0,"LDO off is written"},
        {FEL_PLL,PLL_CHANGED,0,3,0xe1204d00u,0,"a changed word during the wait"},
        {0xe1204d00u,PLL_LOCKS,1,3,0xf9204d00u,0,"left enabled by a failed start: relocked"},
        {0xf1204d00u,PLL_LOCKS,1,3,0xf9204d00u,0,"left enabled and locked: relocked"},
        {0xc1204d00u,PLL_LOCKS,1,2,0xf9204d00u,0,"left enabled before lock enable: relocked"},
    };
    for(unsigned row=0;row<sizeof(vendor_cases)/sizeof(vendor_cases[0]);++row) {
        const struct lifetime l={.ack_us=10,.pll=vendor_cases[row].scenario,.pll_entry=vendor_cases[row].entry,
            .edit=vendor_cases[row].edit};
        const struct outcome o=lifetime(&c,&l,0,0,0);
        const unsigned w=vendor_cases[row].writes;
        assert(c.pll_writes==w&&vm->ccu[0x220/4]==vendor_cases[row].final);
        if(vendor_cases[row].ok) {
            /* 0x5e4, reset, bus gate, selector, the PLL table, module gate,
             * VCU pulse. */
            assert(o.result==1&&vm->drain==2&&vm->trigger&&!vm->lock&&c.cold_pulse==2);
            assert(vm->events[0].address==CCU+0x5e4&&vm->events[1].address==CCU+0xa8c&&vm->events[2].address==CCU+0xa8c);
            assert(vm->events[3].address==CCU+0xa88&&vm->events[3].value==0x01000000);
            for(unsigned i=0;i<w;++i)assert(vm->events[4+i].address==CCU+0x220);
            assert(vm->events[4+w].address==CCU+0xa88&&vm->events[4+w].value==0x81000000);
            assert(vm->events[5+w].address==VE+0x804&&vm->events[6+w].address==VE+0x804);
            if(!row)print_holds("vendor-fel",&c);
        }else {
            assert(o.result==-1&&!vm->trigger&&!vm->memory&&!vm->vereads);
            /* A refused entry writes nothing and requests no power. A failed
             * wait leaves the PLL enabled with its output and the module gate
             * closed, and the register mutex free. */
            if(!w)assert(!vm->writes&&o.row==A733_POWER_ROW_IDLE&&!o.owner_ppu_writes);
            else assert(vm->writes==4+w&&!(vm->ccu[0xa88/4]&0x80000000));
            if(c.pll==PLL_NEVER)assert(c.pll_reads==101&&o.rec.lock_reads==0);
            assert(!vm->lock);
        }
        printf("case%u %s: result%d writes%u PLL_writes%u PLL_reads%u PLL_word%08x module%08x lock_reads%u\n",7+row,
            vendor_cases[row].name,o.result,o.writes,c.pll_writes,c.pll_reads,vm->ccu[0x220/4],vm->ccu[0xa88/4],
            o.rec.lock_reads);
    }
    /* Domain-2 and gate cases at the vendor point from the FEL words. */
    static const struct {uint32_t policy,status;void (*edit)(struct model *);unsigned key_zero;int ok;const char *name;}
    domain_cases[]={
        {8,8,0,0,1,"domain left ON (FEL gates, AHB gate open): adopted, no PCK write"},
        {8,0,0,0,0,"power-on in progress: refused by the owner, no decoder write"},
        {0,8,0,0,0,"power-off in progress: refused by the owner, no decoder write"},
        {8,8,reset_released,0,0,"ON with reset released: entry refused, no request"},
        {8,8,mbus_clear,0,0,"MBUS master gate clear after ON: refused at A1 after the 0x5e4 open"},
        {0,0,ahb_clear,0,0,"AHB gate clear after ON: refused at A1 after the 0x5e4 open"},
        {0,0,0,1,1,"0x5e4 key bits read zero: admitted, readback recorded"},
        {0,0,mclk_open,0,1,"0x5e4 already open with the key: no gate write"},
    };
    for(unsigned k=0;k<sizeof(domain_cases)/sizeof(domain_cases[0]);++k) {
        const struct lifetime l={.policy=domain_cases[k].policy,.status=domain_cases[k].status,.ack_us=10,.pll=PLL_LOCKS,
            .pll_entry=FEL_PLL,.key_zero=domain_cases[k].key_zero,.edit=domain_cases[k].edit};
        const struct outcome o=lifetime(&c,&l,0,0,0);
        if(domain_cases[k].ok) {
            assert(o.result==1&&vm->trigger&&c.cold_pulse==2&&o.row==A733_POWER_ROW_ON);
            if(domain_cases[k].policy==8)assert(!o.owner_ppu_writes&&!o.owner_commands);
            if(domain_cases[k].key_zero)assert(o.rec.mclk_written==0x40306&&o.rec.mclk_readback==0x40004);
            if(domain_cases[k].edit==mclk_open)assert(vm->events[0].address!=CCU+0x5e4&&!o.rec.mclk_written);
        }else {
            assert(o.result==-1&&!vm->trigger&&!vm->vereads&&!vm->lock);
            if(k==1||k==2)assert(!vm->writes&&o.row==A733_POWER_ROW_REFUSED&&!o.owner_ppu_writes);
            if(k==3)assert(!vm->writes&&o.row==A733_POWER_ROW_IDLE);
            if(k==4||k==5)assert(vm->writes==1&&vm->events[0].address==CCU+0x5e4&&!(vm->ccu[0xa8c/4]&(1u<<18)));
        }
        printf("domain%u %s: result%d writes%u owner_ppu_writes%u\n",k,domain_cases[k].name,o.result,o.writes,
            o.owner_ppu_writes);
    }
    /* Two lifetimes: the first from domain 0/0 and the FEL words, ending in
     * the quiesce; the second from the domain words it left, once with the
     * CCU as the quiesce left it and once with the CCU at the FEL words. */
    {
        static uint32_t after[2048];uint32_t ppu[2];
        const struct lifetime l={.ack_us=10,.pll=PLL_LOCKS,.pll_entry=FEL_PLL};
        const struct outcome first=lifetime(&c,&l,0,0,1);
        assert(first.result==1&&first.owner_commands==1);
        memcpy(after,vm->ccu,sizeof(after));ppu[0]=d.ppu[2][0]&15;ppu[1]=d.ppu[2][2]&15;
        assert(ppu[0]==8&&ppu[1]==8&&after[0x220/4]==0x01204d00u&&after[0xa88/4]==0x01000000u&&
            !(after[0xa8c/4]&0x40004)&&after[0x5e4/4]==0x40306);
        const unsigned quiesce_writes=vm->writes-first.writes;
        printf("lifetime1: result%d writes%u quiesce_writes%u PLL%08x module%08x bus%08x mclk%08x domain%u/%u\n",
            first.result,first.writes,quiesce_writes,after[0x220/4],after[0xa88/4],after[0xa8c/4],after[0x5e4/4],ppu[0],ppu[1]);
        const struct outcome kept=lifetime(&c,&l,after,ppu,1);
        assert(kept.result==1&&!kept.owner_ppu_writes&&!kept.owner_commands&&c.pll_writes==4+4);
        assert(vm->events[0].address==CCU+0xa8c); /* 0x5e4 already carries the key */
        printf("lifetime2 (CCU as quiesced): result%d writes%u owner_ppu_writes%u\n",kept.result,kept.writes,
            kept.owner_ppu_writes);
        const struct outcome reset=lifetime(&c,&l,0,ppu,1);
        assert(reset.result==1&&!reset.owner_ppu_writes&&vm->events[0].address==CCU+0x5e4);
        printf("lifetime2 (CCU at the FEL words): result%d writes%u owner_ppu_writes%u\n",reset.result,reset.writes,
            reset.owner_ppu_writes);
    }
    /* The OS service's open and close from the FEL words, domain 2 OFF and then adopted ON. */
    {
        const struct lifetime l={.ack_us=10,.pll=PLL_LOCKS,.pll_entry=FEL_PLL,.service=1};
        const struct outcome first=lifetime(&c,&l,0,0,1);
        assert(first.result==1&&first.owner_commands==1&&vm->ccu[0x220/4]==0x01204d00u&&vm->ccu[0x5e4/4]==0x40306);
        const uint32_t ppu[2]={d.ppu[2][0]&15,d.ppu[2][2]&15};
        const struct outcome kept=lifetime(&c,&l,0,ppu,1);
        assert(kept.result==1&&!kept.owner_commands&&!kept.owner_ppu_writes);
        printf("service: result%d writes%u, adopted result%d writes%u\n",first.result,first.writes,kept.result,kept.writes);
    }
    policy();
    puts("PASS: decoder cold start on the actual owner: entry before the row request, ON from 0/0 or adopted, refusals "
         "with no decoder write, one 0x5e4 key open before A1, the three critical gates required at A1, one acquire call "
         "from ON to started, PLL_VE0 at the vendor 624 MHz from the FEL word and fifteen other entry and lock cases, "
         "quiesce to an admitted entry state over two lifetimes, the native write allowlist. Model only.");
    return 0;
}
