/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "a733_video_platform.h"
#include "a733_range.h"
#define CCU UINT32_C(0x02002000)
#define PCK UINT32_C(0x07062000)
#define R_CCU UINT32_C(0x07010000)
#define VE UINT32_C(0x01c0e000)
#define GIC UINT32_C(0x03400000)
#define BIT(n) (UINT32_C(1)<<(n))
#define fail a733_video_unit_fail
#define timely a733_video_unit_timely
#define held a733_video_unit_held
#define lock a733_video_unit_lock
#define unlock a733_video_unit_unlock
#define rd a733_video_unit_read
#define check a733_video_unit_check
#define update a733_video_unit_update
static struct a733_video_unit_config unit_config(const struct a733_video_platform_config *c)
{
    struct a733_video_unit_config u={.clocks=c->clocks,.clock=c->clock,.power=c->power,.pck=c->pck,
        .power_client=c->power_client,.owner=c->owner,.handoff=c->handoff,.module=c->module,.module_hz=c->module_hz,
        /* A cold start at the vendor operating point programs PLL_VE0. */
        .program_pll=c->start==A733_VIDEO_COLD_NS_ROM&&(c->module&0x0700001fu)==A733_VIDEO_VENDOR_MODULE&&
            c->module_hz==A733_VIDEO_VENDOR_HZ};
    for(unsigned i=0;i<3;++i){u.fabric[i]=c->fabric[i];u.fabric_hz[i]=c->fabric_hz[i];}
    return u;
}
int a733_video_platform_scopes(const struct a733_video_platform_config *c,a733_clock_mask_t *out)
{
    if(!c||(c->start!=A733_VIDEO_INHERITED&&c->start!=A733_VIDEO_COLD_NS_ROM))return 0;
    const struct a733_video_unit_config u=unit_config(c);
    return a733_video_unit_scopes(&a733_video_decoder_unit,&u,out);
}
a733_clock_mask_t a733_video_platform_exclusive(const struct a733_video_platform_config *c)
{
    a733_clock_mask_t shared;
    if(!a733_video_platform_scopes(c,&shared))return 0;
    const struct a733_video_unit_config u=unit_config(c);
    return a733_video_unit_exclusive(&a733_video_decoder_unit,&u);
}
static int decoder_init(struct a733_video_decoder *p,const struct a733_video_platform_config *c,
    const struct a733_video_unit_ops *o,void *cookie,
    int (*admit)(void *,const struct a733_video_platform_config *,const void *,uint64_t,uint64_t *),
    int (*memory)(void *,uint64_t,const void *,uint64_t),void *context)
{
    if(!c||(c->start!=A733_VIDEO_INHERITED&&c->start!=A733_VIDEO_COLD_NS_ROM)||p->unit.initialized||
       (c->start==A733_VIDEO_COLD_NS_ROM&&(!c->row||c->row->domain!=2)))return 0;
    const struct a733_video_platform_config config=*c;
    const struct a733_video_unit_config u=unit_config(&config);
    if(!a733_video_unit_init(&p->unit,&a733_video_decoder_unit,&u,o,cookie))return 0;
    p->config=config;p->admit=admit;p->memory=memory;p->context=context;
    p->version=p->reset_until=0;p->admitted=p->checked=p->mapped=p->prepared=p->step=p->cold_step=p->requested=0;
    p->cold=(struct a733_video_cold_record){0};
    /* The page walks run here, once per boot, so the first hold makes none. Its result is decided again, without
     * a walk, at each access; a host build has no pages to check. */
    (void)a733_video_unit_native_map();
    return 1;
}
int a733_video_decoder_init(struct a733_video_decoder *p,const struct a733_video_platform_config *c,
    const struct a733_video_unit_ops *o,void *cookie)
{return p&&o&&decoder_init(p,c,o,cookie,0,0,0);}
/* NS_BROM at0x17000, published sun60iw2p1 revision. These instruction
 * observations bind the literal to the FEL identity constructor. Never execute
 * ROM code or accept caller-provided bytes as native identity evidence. */
static const struct {uint32_t address,word;} rom_identity[]={
    {0x19a88,0xe3a02008},{0x19a8c,0xe28f1c02},
    {0x19a90,0xe28d0004},{0x19a94,0xebfff6ca},
    {0x19a98,0xe59f0200},{0x19a9c,0xe58d000c},
    {0x19aa0,0xe3a00001},{0x19aa4,0xe58d0010},
    {0x19aa8,0xe1cd01b4},{0x19abc,0xe59f01e0},
    {0x19ac0,0xe58d0018},{0x19c94,0x53555741},
    {0x19c98,0x58454642},{0x19ca0,0x00190300},
    {0x19ca4,0x000ee800}
};
/* The ROM identity, before the entry hold and outside the register lock:
 * the boot ROM is immutable, so its fifteen words need no mutex. The
 * deadline is checked once, before the reads. A word that differs refuses
 * the start; a read the native allowlist or mapping refuses is uncertain,
 * as in a hold. */
static int rom_matches(struct a733_video_decoder *p)
{
    struct a733_video_unit *u=&p->unit;
    if(u->failed||u->locked||!a733_video_unit_timely(u))return 0;
    for(unsigned i=0;i<sizeof(rom_identity)/sizeof(rom_identity[0]);++i) {
        uint32_t w;
        if(a733_video_unit_read_unlocked(u,rom_identity[i].address,&w)!=1||w!=rom_identity[i].word)return 0;
    }
    return 1;
}
/* Entry, under one register-lock hold, before the domain-2 row request:
 * the fabric and parent clocks, reset asserted, bus and
 * module gates closed, IRQ81 quiet, and CM_VE_DEC_CFG (0x1b10) with its
 * module mode disabled (bit 0) and its status field (bits 17:16) at 0b10,
 * POWER_ON (U-Boot plat-sun60iw2p1/clock_autogen.h lines 4760-4768). Both
 * port 3-3 at FEL and port 3-1 at OS-image entry read 0x20000 with the module
 * gate closed; the powered baseline requires the same word. The three critical gates are recorded in any state. No PCK600
 * register is read: the owner's coherence rules admit the domain. */
/* The decoder's three critical gates, CCU offset and bit: AHB 0x5c0 bit 0, MBUS 0x5e0 bit 14, MBUS clock 0x5e4 bit 18. */
static const uint32_t gate[3]={0x5c0,0x5e0,0x5e4},bit[3]={BIT(0),BIT(14),BIT(18)};
static int cold_entry(struct a733_video_decoder *p)
{
    struct a733_video_unit *u=&p->unit;
    for(unsigned i=0;i<3;++i)if(rd(u,CCU+gate[i],&p->cold.entry_gates[i])!=1)return 0;
    /* A programmed PLL_VE0 may be disabled, and the module selector and
     * divider are then written later; the module gate must be closed. */
    return a733_video_unit_clocks_entry(u)&&
        (u->config.program_pll?check(u,CCU+0xa88,BIT(31),0):check(u,CCU+0xa88,0x8700001f,p->config.module&~BIT(31)))&&
        check(u,CCU+0xa8c,BIT(18)|BIT(2),0)&&check(u,CCU+0x1b10,0x30001,0x20000)&&
        a733_video_unit_irq_quiet(u);
}
/* The decoder's MBUS clock gate, 0x5e4 bit 18, is CLK_IS_CRITICAL
 * (ccu-sun60iw2.c lines 688-692) and the board's FEL state has it clear
 * (0x5e4 = 0x4 on port 3-3). It is opened once, as the kernel's critical-clock
 * registration does (ccu_gate.c ccu_gate_helper_enable, lines 213-240): a
 * read-modify-write that ORs in the key value 0x40302, then a readback. The
 * kernel says the key bits read as zero; the manual (4.1.6.84) documents bits
 * 1, 8 and 9 as the VE encoder, CSI and ISP MBUS clock gates. The readback
 * therefore requires bit 18 and every bit outside the key unchanged, and the
 * word is recorded. This is the decoder's only gate write; 0x5c0 bit 0 and
 * 0x5e0 bit 14 are only read. */
#define MCLK_KEY UINT32_C(0x40302)
static int open_mclk(struct a733_video_decoder *p)
{
    struct a733_video_unit *u=&p->unit;uint32_t old,actual;
    if(rd(u,CCU+0x5e4,&old)!=1)return 0;
    if((old&MCLK_KEY)==MCLK_KEY)return 1;
    p->cold.mclk_written=old|MCLK_KEY;
    if(a733_video_unit_write(u,CCU+0x5e4,4,p->cold.mclk_written)!=1||rd(u,CCU+0x5e4,&actual)!=1)return 0;
    p->cold.mclk_readback=actual;
    return (actual&~(MCLK_KEY&~BIT(18)))==(p->cold.mclk_written&~(MCLK_KEY&~BIT(18)));
}
/* Hold A, after the owner's ON answer: open the MBUS clock gate, then A1
 * requires all three critical gates set before any other write (a refusal
 * leaves the engine in reset), then reset release, bus gate and the PLL_VE0
 * table up to lock enable. Reset release and the bus gate follow cedar_ve.c
 * (enable_cedar_hw_clk_reset lines 368-386, after pm_runtime_get_sync, then
 * enable_cedar_hw_clk_module lines 416-478); the module gate opens last, in
 * hold B, with the parent clk_set_parent chose (line 1789). setup_ve_freq
 * changes the rate with the module clock disabled too (lines 351-366). */
static int hold_a(struct a733_video_decoder *p)
{
    struct a733_video_unit *u=&p->unit;
    if(!open_mclk(p))return 0;
    for(unsigned i=0;i<3;++i)if(rd(u,CCU+gate[i],&p->cold.gates[i])!=1)return 0;
    for(unsigned i=0;i<3;++i)if(!(p->cold.gates[i]&bit[i]))return 0;
    return update(u,CCU+0xa8c,BIT(18),BIT(18))&&update(u,CCU+0xa8c,BIT(2),BIT(2))&&
        a733_video_unit_pll_start(u);
}
/* The VE top reset pulse as the vendor writes it (set_vcu_en_regmode_to1_to0,
 * cedar_ve.c lines 389-405): read, write bits 0 and 4 set, read, write them
 * clear, with no readback check. No source documents how the pulse bits read
 * back (the manual has no VE register section), so the two words read are
 * kept, through vcu, for the board to show. */
static int vcu_pulse(struct a733_video_unit *u,uint32_t *vcu)
{
    return a733_video_unit_rmw(u,VE+0x804,0x11,0x11,vcu?&vcu[0]:NULL)&&
        a733_video_unit_rmw(u,VE+0x804,0x11,0,vcu?&vcu[1]:NULL);
}
/* Hold B, after the lock wait: the PLL output gate and the module gate,
 * the powered baseline, then the VCU pulse that the A733 kernel's cold
 * startup always makes before any compound-ID lookup (called at cedar_ve.c
 * line 535). Immutable ROM/resource admission authorizes only this reset. */
static int hold_b(struct a733_video_decoder *p)
{
    struct a733_video_unit *u=&p->unit;
    return a733_video_unit_pll_open(u)&&update(u,CCU+0xa88,BIT(31),BIT(31))&&a733_video_unit_baseline(u)&&
        vcu_pulse(u,p->cold.vcu);
}
/* The cold start's phases. From the row's ON answer to a started decoder it
 * runs in one acquire call: hold A, the lock wait without the register
 * mutex, hold B. A busy register mutex (effect-free) returns pending and
 * the next call resumes at the same phase. A phase refused on a value read
 * completely (a check or readback that differs) releases the mutex before
 * failing the unit; an uncertain access, which the core has already marked,
 * retains it. */
enum { COLD_ENTRY, COLD_POWER, COLD_HOLD_A, COLD_WAIT, COLD_HOLD_B, COLD_DONE };
static int held_phase(struct a733_video_decoder *p,int (*phase)(struct a733_video_decoder *),uint64_t *at)
{
    struct a733_video_unit *u=&p->unit;
    int r=lock(u);if(r!=1)return r;
    if(p->cold_step==COLD_ENTRY)p->cold.entry_at=u->last_now;
    if(!phase(p))return a733_video_unit_refuse_held(u);
    if(unlock(u)!=1)return -1;
    *at=u->last_now;++p->cold_step;return 1;
}
static int cold(struct a733_video_decoder *p)
{
    struct a733_video_unit *u=&p->unit;
    for(;;) {
        int r=1;
        switch(p->cold_step) {
        case COLD_ENTRY:
            if(!p->rom_checked){if(!rom_matches(p))return fail(u);p->rom_checked=1;}
            r=held_phase(p,cold_entry,&p->cold.entered_at);break;
        case COLD_POWER: {
            if(!p->requested) {
                if(!a733_power_domain_request(p->config.row,1))return fail(u);
                p->requested=1;
            }
            const enum a733_power_row_state s=a733_power_domain_state(p->config.row);
            if(s==A733_POWER_ROW_PENDING)return 0;
            if(s!=A733_POWER_ROW_ON)return fail(u);
            uint64_t n;if(a733_video_unit_now(u,&n)!=1)return -1;
            p->cold.on_at=n;++p->cold_step;break;
        }
        case COLD_HOLD_A:r=held_phase(p,hold_a,&p->cold.hold_a_at);break;
        case COLD_WAIT: {
            /* locked_at follows the settle, which the wait spins on the per-core clock. */
            if(!a733_video_unit_pll_wait(u))return fail(u);
            uint64_t n;if(a733_video_unit_now(u,&n)!=1)return -1;
            p->cold.lock_reads=u->pll_reads;p->cold.locked_at=n;++p->cold_step;break;
        }
        case COLD_HOLD_B:r=held_phase(p,hold_b,&p->cold.hold_b_at);break;
        default:return 1;
        }
        if(r!=1)return r;
    }
}
/* The start: the cold start, or the inherited powered baseline under one hold. */
static int start(struct a733_video_decoder *p)
{
    struct a733_video_unit *u=&p->unit;
    if(p->config.start==A733_VIDEO_COLD_NS_ROM)return cold(p);
    int r=lock(u);if(r!=1)return r;
    if(!a733_video_unit_baseline(u))return a733_video_unit_refuse_held(u);
    return unlock(u);
}
static int acquire(struct a733_video_decoder *p,const void *j,uint64_t d,uint64_t *token)
{
    struct a733_video_unit *u=&p->unit;
    /* A quiesced decoder stays closed for the rest of its power lifetime. */
    if(!u->initialized||u->failed||p->quiesced||!token||!d||(u->deadline&&u->deadline!=d))return -1;
    u->deadline=d;
    if(!timely(u))return fail(u);
    if(!p->admitted) {
        uint64_t previous=u->token;
        int r=p->admit(p->context,&p->config,j,d,&u->token);
        *token=u->token;
        if(!u->token||(previous&&previous!=u->token)||(r!=0&&r!=1)||!timely(u))return fail(u);
        if(!r)return 0;
        p->admitted=1;
    }
    *token=u->token;
    if(p->checked)return 1;
    int r=start(p);if(r!=1)return r;
    p->checked=1;return 1;
}
/* A failed start wrote a register, or keeps the register lock: a late access or lock operation fails the unit inside
 * its hold without releasing the lock (a733_video_unit_refuse_held), even in the entry hold, which writes nothing.
 * Only a close ends either; the fault close continues in the held lock. */
static int written(const struct a733_video_decoder *p)
{return p->unit.locked||(p->config.start==A733_VIDEO_COLD_NS_ROM&&p->cold_step>=COLD_HOLD_A);}
/* An open that ends refused has written nothing and holds no lock: it drops its deadline, so the next open runs with
 * its own. A refusal that only the deadline caused (late, with every access certain) also clears the failure; any
 * other refusal leaves the unit failed. */
static int refused(struct a733_video_decoder *p)
{
    struct a733_video_unit *u=&p->unit;
    if(u->late&&!u->uncertain){u->failed=0;u->late=0;}
    u->deadline=0;
    return A733_VIDEO_OPEN_REFUSED;
}
int a733_video_decoder_open(struct a733_video_decoder *p,uint64_t d)
{
    if(!p)return A733_VIDEO_OPEN_REFUSED;
    struct a733_video_unit *u=&p->unit;
    if(p->admit||!u->initialized||u->failed||p->quiesced||!d||(u->deadline&&u->deadline!=d))return A733_VIDEO_OPEN_REFUSED;
    u->deadline=d;
    int r=timely(u)?1:fail(u);
    if(r==1&&!p->checked&&(r=start(p))==1)p->checked=1;
    if(r==1&&!p->version) {
        /* The identity block, readable once the decoder is started. */
        if((r=lock(u))==1)r=a733_video_unit_identity(u,&p->version)?unlock(u):a733_video_unit_refuse_held(u);
    }
    if(r>=0)return r;
    return written(p)?A733_VIDEO_OPEN_FAULT:refused(p);
}
int a733_video_decoder_cancel(struct a733_video_decoder *p)
{return !p||!p->unit.initialized||written(p)?-1:refused(p);}
/* The close in the vendor order, under the held mutex, each write read back. */
static int close_steps(struct a733_video_unit *u)
{
    return update(u,CCU+0xa88,BIT(31),0)&&update(u,CCU+0xa8c,BIT(2),0)&&update(u,CCU+0xa8c,BIT(18),0)&&
        a733_video_unit_pll_stop(u);
}
int a733_video_decoder_close(struct a733_video_decoder *p,uint64_t d,unsigned faulted)
{
    if(!p||faulted>1)return -1;
    struct a733_video_unit *u=&p->unit;
    if(!u->initialized||!d||p->quiesced||(faulted?u->uncertain:u->failed||u->locked))return -1;
    /* After a fault: a refusal released the mutex; a late access or check kept it, and the close continues in that
     * hold. */
    const unsigned held_on=faulted&&u->locked;
    if(faulted){u->failed=0;u->late=0;}
    u->deadline=d;
    if(!held_on) {
        int r=lock(u);
        if(r!=1){if(!r&&faulted)u->failed=1;return r;}
    } else if(a733_video_unit_now(u,&(uint64_t){0})!=1)return -1;
    p->cold.close_at=u->last_now;
    if(!close_steps(u))return a733_video_unit_refuse_held(u);
    if(unlock(u)!=1)return -1;
    p->cold.closed_at=u->last_now;
    p->checked=p->prepared=0;p->quiesced=1;u->failed=faulted;
    return 1;
}
/* VeReset policy45/52 delegates external reset; policy51 uses top+4 bit16.
 * Remaining updates combine package RMW operations with the same final fields.
 * A reset pulse is NOT a drain mechanism: authority must already prove drain.
 */
static int prep_step(struct a733_video_decoder *p,unsigned codec)
{
    struct a733_video_unit *u=&p->unit;
    unsigned local=p->version==UINT64_C(0x3331000021320);
    switch(p->step) {
    case 0:return update(u,VE,15,7);
    case 1:
        if(!update(u,local?VE+4:CCU+0xa8c,local?BIT(16):BIT(18),local?BIT(16):0))return 0;
        if(local)return 1;
        /* The pulse is anchored on a clock read after the assert's write, so the release (case 2, at least 11 µs
         * later) gives a pulse of at least 10 µs however long the write took. */
        {uint64_t n;if(a733_video_unit_now(u,&n)!=1||!a733_deadline_after(n,11,&p->reset_until))return 0;}
        return 1;
    case 2:return update(u,local?VE+4:CCU+0xa8c,local?BIT(16):BIT(18),local?0:BIT(18));
    case 3:return local?1:a733_video_unit_rmw(u,VE+0x804,0x11,0x11,NULL);
    case 4:return local?1:a733_video_unit_rmw(u,VE+0x804,0x11,0,NULL);
    case 5:return local?1:update(u,VE,0x130000,0x130000);
    case 6:return update(u,VE,0x2c00101f,codec);
    case 7:return update(u,VE+0x1c,0x100,0);
    case 8:return update(u,VE+0xc4,UINT32_MAX,0);
    case 9:return update(u,VE+0xc8,UINT32_MAX,0);
    case 10:return update(u,VE+0xcc,UINT32_MAX,0);
    case 11:return update(u,VE+0xe8,UINT32_MAX,0);
    case 12:return update(u,VE+0xec,0x30077,0);
    case 13:return update(u,VE+0x50,15,0);
    case 14:return update(u,VE+0x54,UINT32_MAX,0);
    case 15:return p->version==UINT64_C(0x3052000012011)?1:update(u,VE,0x400,0x400);
    case 16:return update(u,VE,0x600000,0); /* width <=1920 */
    case 17:return update(u,VE,0x130000,0x130000); /* Package DRAM mode5. */
    default:return 0;
    }
}
static int prepare(struct a733_video_decoder *p,uint64_t token,const void *j,uint64_t d,unsigned codec)
{
    struct a733_video_unit *u=&p->unit;
    if(u->failed||!p->checked||token!=u->token||d!=u->deadline||!timely(u))return fail(u);
    if(p->prepared)return 1;
    if(!p->version) {
        int r=lock(u);if(r!=1)return r;
        if(!a733_video_unit_identity(u,&p->version))return a733_video_unit_refuse_held(u);
        if(unlock(u)!=1)return -1;
        return 0;
    }
    if(!p->mapped) {
        int h=held(u);if(h!=1)return h;
        int r=p->memory(p->context,token,j,d);
        if((r!=0&&r!=1)||!timely(u))return fail(u);
        if(r==1)p->mapped=1;
        return 0;
    }
    if(p->step==2&&u->last_now<p->reset_until)return 0;
    int r=lock(u);if(r!=1)return r;
    if(!prep_step(p,codec))return a733_video_unit_refuse_held(u);
    ++p->step;
    if(unlock(u)!=1)return -1;
    if(p->step==18)p->prepared=1;
    return p->prepared?1:0;
}
/* Identity reads are allowed after start; codec reads only after preparation. */
static int codec_read(struct a733_video_decoder *p,uint64_t token,uint32_t off,uint32_t *out,uint64_t d,int allowed)
{
    struct a733_video_unit *u=&p->unit;
    if(u->failed||!p->checked||token!=u->token||d!=u->deadline||!out)return fail(u);
    if(off!=0xf0&&off!=0xe0&&off!=0xe4&&(!p->prepared||!allowed))return fail(u);
    if(lock(u)!=1)return fail(u);
    if(rd(u,VE+off,out)!=1)return -1;
    return unlock(u);
}
static int codec_write(struct a733_video_decoder *p,uint64_t token,uint32_t off,unsigned width,
    uint32_t value,uint64_t d,int allowed)
{
    struct a733_video_unit *u=&p->unit;
    if(u->failed||!p->prepared||token!=u->token||d!=u->deadline||!allowed)return fail(u);
    if(lock(u)!=1)return fail(u);
    if(a733_video_unit_write(u,VE+off,width,value)!=1)return -1;
    return unlock(u);
}
static int retained_copy(struct a733_video_decoder *p,uint64_t token,uint64_t d,
    int (*copy)(void *,uint64_t,uint64_t),void *cookie)
{
    struct a733_video_unit *u=&p->unit;
    if(!copy||u->failed||!p->prepared||token!=u->token||d!=u->deadline||!timely(u))return -1;
    int h=held(u);if(h!=1)return h;
    int r=copy(cookie,token,d);
    return (r==0||r==1)&&timely(u)?r:fail(u);
}

/* JPEG binding. The memory callback also receives the parsed picture and the
 * output geometry of the current prepare call. */
struct jpeg_memory_job {
    const struct a733_video_jpeg_job *job;
    const struct a733_video_jpeg *picture;
    uint32_t stride,rows;
};
static int jpeg_admit(void *v,const struct a733_video_platform_config *c,const void *j,uint64_t d,uint64_t *t)
{struct a733_video_platform *p=v;return p->ops.admit(p->cookie,c,j,d,t);}
static int jpeg_memory(void *v,uint64_t t,const void *job,uint64_t d)
{
    struct a733_video_platform *p=v;const struct jpeg_memory_job *m=job;
    return p->ops.memory(p->cookie,t,m->job,m->picture,m->stride,m->rows,d);
}
int a733_video_platform_init(struct a733_video_platform *p,
 const struct a733_video_platform_config *c,const struct a733_video_platform_ops *o,void *cookie)
{
    if(!p||!o||!o->admit||!o->memory||!o->drain)return 0;
    const struct a733_video_unit_ops unit={o->now_us,o->enter,o->leave,o->read,o->write,o->spin_now_us};
    const struct a733_video_platform_ops ops=*o;
    if(!decoder_init(&p->decoder,c,&unit,cookie,jpeg_admit,jpeg_memory,p))return 0;
    p->ops=ops;p->cookie=cookie;
    return 1;
}
static int jpeg_now(void *v,uint64_t *out)
{struct a733_video_platform *p=v;return p?a733_video_unit_now(&p->decoder.unit,out):-1;}
static int jpeg_acquire(void *v,const struct a733_video_jpeg_job *j,uint64_t d,uint64_t *token)
{struct a733_video_platform *p=v;return p?acquire(&p->decoder,j,d,token):-1;}
static int jpeg_prepare(void *v,uint64_t token,const struct a733_video_jpeg_job *j,
 const struct a733_video_jpeg *pic,uint32_t stride,uint32_t rows,uint64_t d)
{
    struct a733_video_platform *p=v;const struct jpeg_memory_job job={j,pic,stride,rows};
    return prepare(&p->decoder,token,&job,d,0);
}
static int jpeg_read(void *v,uint64_t token,uint32_t off,uint32_t *out,uint64_t d)
{
    struct a733_video_platform *p=v;
    return codec_read(&p->decoder,token,off,out,d,off==0||off==0x11c||off==0xec);
}
static int jpeg_write_allowed(uint32_t off,unsigned width)
{
    if(width==1)return off==0x11b||off==0x118;
    if(width!=4)return 0;
    switch(off) {
    case 0x1c0:case 0x180:case 0x1cc:case 0x1d0:case 0x1b8:case 0x1d4:
    case 0x1e0:case 0x1e4:case 0x134:case 0x114:case 0xc4:case 0xc8:case 0xec:
    case 0xe8:case 0xcc:case 0x12c:case 0x130:case 0x128:case 0x11c:return 1;
    default:return 0;
    }
}
static int jpeg_write(void *v,uint64_t token,uint32_t off,unsigned width,uint32_t value,uint64_t d)
{
    struct a733_video_platform *p=v;
    return codec_write(&p->decoder,token,off,width,value,d,jpeg_write_allowed(off,width));
}
static int finish(void *v,uint64_t token,uint64_t d)
{
    struct a733_video_platform *p=v;struct a733_video_unit *u=&p->decoder.unit;
    if(u->failed||!p->decoder.prepared||token!=u->token||d!=u->deadline||!timely(u))return fail(u);
    int h=held(u);if(h!=1)return h;
    int r=p->ops.drain(p->cookie,token,d);
    return (r==0||r==1)&&timely(u)?r:fail(u);
}
int a733_video_platform_retained_copy(void *v,uint64_t token,uint64_t d,
    int (*copy)(void *,uint64_t,uint64_t),void *cookie)
{
    struct a733_video_platform *p=v;
    return p?retained_copy(&p->decoder,token,d,copy,cookie):-1;
}
const struct a733_video_jpeg_ops *a733_video_platform_jpeg_ops(void)
{
    static const struct a733_video_jpeg_ops ops={jpeg_now,jpeg_acquire,jpeg_prepare,jpeg_read,jpeg_write,finish};
    return &ops;
}

/* H.264 binding. */
static int h264_write_allowed(uint32_t off)
{
    switch(off){
    case 0xec:case 0xc4:case 0xc8:case 0x200:case 0x204:case 0x208:case 0x20c:
    case 0x210:case 0x21c:case 0x220:case 0x224:case 0x228:case 0x22c:case 0x230:case 0x234:
    case 0x238:case 0x23c:case 0x240:case 0x24c:case 0x250:case 0x254:case 0x260:
    case 0x2e0:case 0x2e4:return 1;
    default:return 0;
    }
}
static int h264_admit(void *v,const struct a733_video_platform_config *c,const void *j,uint64_t d,uint64_t *t)
{struct a733_h264_platform *p=v;return p->ops.admit(p->cookie,c,j,d,t);}
static int h264_memory(void *v,uint64_t t,const void *j,uint64_t d)
{struct a733_h264_platform *p=v;return p->ops.memory(p->cookie,t,j,d);}
int a733_h264_platform_init(struct a733_h264_platform *p,const struct a733_video_platform_config *c,
 const struct a733_h264_platform_ops *o,void *cookie)
{
    if(!p||!o||!o->admit||!o->memory||!o->copied)return 0;
    const struct a733_video_unit_ops unit={o->now_us,o->enter,o->leave,o->read,o->write,0};
    const struct a733_h264_platform_ops ops=*o;
    if(!decoder_init(&p->decoder,c,&unit,cookie,h264_admit,h264_memory,p))return 0;
    p->ops=ops;p->cookie=cookie;p->bound=0;
    return 1;
}
static int h264_same_job(const struct a733_h264_job *a,const struct a733_h264_job *b)
{
    if(a->input!=b->input||a->input_bytes!=b->input_bytes||a->profile!=b->profile)return 0;
    for(unsigned i=0;i<A733_H264_BUFFERS;++i)if(a->buffers[i].iova!=b->buffers[i].iova||
        a->buffers[i].bytes!=b->buffers[i].bytes)return 0;
    return 1;
}
static int h264_clock(void *v,uint64_t *n)
{struct a733_h264_platform *p=v;return p?a733_video_unit_now(&p->decoder.unit,n):-1;}
static int h264_acquire(void *v,const struct a733_h264_job *j,uint64_t d,uint64_t *t)
{
    struct a733_h264_platform *p=v;
    if(!p||!j)return -1;
    if(p->bound&&!h264_same_job(&p->job,j))return fail(&p->decoder.unit);
    if(!p->bound){p->job=*j;p->bound=1;}
    return acquire(&p->decoder,&p->job,d,t);
}
static int h264_prepare(void *v,uint64_t t,const struct a733_h264_job *j,uint64_t d)
{
    struct a733_h264_platform *p=v;
    if(!p||!j||!p->bound||!h264_same_job(&p->job,j))return -1;
    return prepare(&p->decoder,t,&p->job,d,1);
}
static int h264_read(void *v,uint64_t t,uint32_t off,uint32_t *x,uint64_t d)
{
    struct a733_h264_platform *p=v;
    return codec_read(&p->decoder,t,off,x,d,off==0||off==0x1c||off==0xb0||off==0xec||
        off==0x228||off==0x22c||off==0x5c||off==0x804);
}
static int h264_write(void *v,uint64_t t,uint32_t off,uint32_t x,uint64_t d)
{struct a733_h264_platform *p=v;return codec_write(&p->decoder,t,off,4,x,d,h264_write_allowed(off));}
static int h264_copied(void *v,uint64_t t,uint64_t d)
{struct a733_h264_platform *p=v;return retained_copy(&p->decoder,t,d,p->ops.copied,p->cookie);}
const struct a733_h264_ops *a733_video_platform_h264_ops(void)
{
    static const struct a733_h264_ops ops={h264_clock,h264_acquire,h264_prepare,h264_read,h264_write,h264_copied};
    return &ops;
}
/* HEVC binding. Writes cover the H.265 engine registers programmed by the
 * controller and the top-level output format registers it shares with H.264. */
static int hevc_write_allowed(uint32_t off)
{
    switch(off){
    case 0xc4:case 0xc8:case 0xcc:case 0xe8:case 0xec:
    case 0x500:case 0x504:case 0x508:case 0x50c:case 0x510:case 0x514:case 0x518:
    case 0x520:case 0x524:case 0x528:case 0x52c:case 0x530:case 0x534:case 0x538:case 0x53c:
    case 0x540:case 0x544:case 0x548:case 0x54c:case 0x550:case 0x554:case 0x558:case 0x55c:
    case 0x560:case 0x564:case 0x568:case 0x56c:case 0x580:case 0x5e0:case 0x5e4:return 1;
    default:return 0;
    }
}
static int hevc_admit(void *v,const struct a733_video_platform_config *c,const void *j,uint64_t d,uint64_t *t)
{struct a733_hevc_platform *p=v;return p->ops.admit(p->cookie,c,j,d,t);}
static int hevc_memory(void *v,uint64_t t,const void *j,uint64_t d)
{struct a733_hevc_platform *p=v;return p->ops.memory(p->cookie,t,j,d);}
int a733_hevc_platform_init(struct a733_hevc_platform *p,const struct a733_video_platform_config *c,
 const struct a733_hevc_platform_ops *o,void *cookie)
{
    if(!p||!o||!o->admit||!o->memory||!o->copied)return 0;
    const struct a733_video_unit_ops unit={o->now_us,o->enter,o->leave,o->read,o->write,0};
    const struct a733_hevc_platform_ops ops=*o;
    if(!decoder_init(&p->decoder,c,&unit,cookie,hevc_admit,hevc_memory,p))return 0;
    p->ops=ops;p->cookie=cookie;p->bound=p->copy_picture=0;
    return 1;
}
static int hevc_same_job(const struct a733_hevc_job *a,const struct a733_hevc_job *b)
{
    /* The plan holds only uint32_t fields. */
    const uint32_t *x=(const uint32_t *)&a->plan,*y=(const uint32_t *)&b->plan;
    if(a->input!=b->input||a->input_bytes!=b->input_bytes)return 0;
    for(unsigned i=0;i<sizeof(a->plan)/sizeof(*x);++i)if(x[i]!=y[i])return 0;
    for(unsigned i=0;i<HEVC_BUFFERS;++i)if(a->buffers[i].iova!=b->buffers[i].iova||
        a->buffers[i].bytes!=b->buffers[i].bytes)return 0;
    return 1;
}
static int hevc_clock(void *v,uint64_t *n)
{struct a733_hevc_platform *p=v;return p?a733_video_unit_now(&p->decoder.unit,n):-1;}
static int hevc_acquire(void *v,const struct a733_hevc_job *j,uint64_t d,uint64_t *t)
{
    struct a733_hevc_platform *p=v;
    if(!p||!j)return -1;
    if(p->bound&&!hevc_same_job(&p->job,j))return fail(&p->decoder.unit);
    if(!p->bound){p->job=*j;p->bound=1;}
    return acquire(&p->decoder,&p->job,d,t);
}
/* Decoder mode 4 selects the H.265 engine (Cedrus VE_MODE_DEC_H265). */
static int hevc_prepare(void *v,uint64_t t,const struct a733_hevc_job *j,uint64_t d)
{
    struct a733_hevc_platform *p=v;
    if(!p||!j||!p->bound||!hevc_same_job(&p->job,j))return -1;
    return prepare(&p->decoder,t,&p->job,d,4);
}
static int hevc_read(void *v,uint64_t t,uint32_t off,uint32_t *x,uint64_t d)
{struct a733_hevc_platform *p=v;return codec_read(&p->decoder,t,off,x,d,off==0x538||off==0x53c);}
static int hevc_write(void *v,uint64_t t,uint32_t off,uint32_t x,uint64_t d)
{struct a733_hevc_platform *p=v;return codec_write(&p->decoder,t,off,4,x,d,hevc_write_allowed(off));}
static int hevc_copy(void *v,uint64_t t,uint64_t d)
{struct a733_hevc_platform *p=v;return p->ops.copied(p->cookie,t,p->copy_picture,d);}
static int hevc_copied(void *v,uint64_t t,unsigned picture,uint64_t d)
{
    struct a733_hevc_platform *p=v;
    if(!p||picture>1)return -1;
    p->copy_picture=picture;
    return retained_copy(&p->decoder,t,d,hevc_copy,p);
}
const struct a733_hevc_ops *a733_video_platform_hevc_ops(void)
{
    static const struct a733_hevc_ops ops={hevc_clock,hevc_acquire,hevc_prepare,hevc_read,hevc_write,hevc_copied};
    return &ops;
}
int a733_video_platform_native_readable(uintptr_t a)
{
    for(unsigned i=0;i<sizeof(rom_identity)/sizeof(rom_identity[0]);++i)
        if(a==rom_identity[i].address)return 1;
    if(a==0x07090160||a==R_CCU+0x1ac||a==PCK||a==PCK+8)return 1;
    if(a>=CCU&&a<CCU+0x2000) switch(a-CCU) {
    case 0:case 0xa0:case 0xa8:case 0xac:case 0x220:case 0x228:case 0x22c:
    case 0x240:case 0x248:case 0x24c:case 0x500:case 0x580:case 0x588:
    case 0x5c0:case 0x5e0:case 0x5e4:case 0xa88:case 0xa8c:case 0x1908:case 0x1918:case 0x1988:case 0x1998:case 0x1b10:return 1;
    default:return 0;
    }
    if(a==GIC||a==GIC+0x108||a==GIC+0x208||a==GIC+0x308||a==GIC+0xc14)return 1;
    if(a>=VE&&a<VE+0x1000) switch(a-VE) {
    case 0:case 4:case 0x1c:case 0x50:case 0x54:case 0xc4:case 0xc8:case 0xcc:
    case 0xe0:case 0xe4:case 0xe8:case 0xec:case 0xf0:case 0x11c:case 0x804:
    case 0xb0:case 0x228:case 0x22c:case 0x5c:case 0x538:case 0x53c:return 1;
    default:return 0;
    }
    return 0;
}
int a733_video_platform_native_read(void *cookie,uintptr_t a,uint32_t *out)
{
    (void)cookie;
    if(!out||!a733_video_platform_native_readable(a))return -1;
    return a733_video_unit_native_read(a,out);
}
/* PLL_VE0 feeds three module clocks (ccu-sun60iw2.c lines 965-1015): the
 * encoder's at mux 0, the decoder's at mux 1 and the NPU's at mux 4. */
static const struct {uint32_t module,mux;} pll_ve0_consumers[]={{0xa80,0},{0xa88,1},{0xb00,4}};
int a733_video_platform_write_policy(uintptr_t a,unsigned width,uint32_t value,
    int (*read)(void *,uintptr_t,uint32_t *),void *cookie,uint32_t *owned)
{
    uint32_t mask=0;
    if(!owned)return -1;
    if(a==CCU+0xa8c)mask=BIT(18)|BIT(2);
    if(a==CCU+0xa88)mask=BIT(31);
    /* Closing the decoder's module gate changes bit 31 alone. */
    if(a==CCU+0xa88&&!(value&BIT(31))) {
        uint32_t m;
        if(width!=4||read(cookie,CCU+0xa88,&m)!=1)return width!=4?0:-1;
        if(m&BIT(31)) {
            if((m^value)!=BIT(31))return 0;
            *owned=BIT(31);return 1;
        }
    }
    /* The PLL_VE0 enable table (bit 24 kept) only while no consumer runs on
     * PLL_VE0, and the decoder's selector and divider only while its module
     * gate is closed: no write retunes a clock a running module uses. */
    if(a==CCU+0x220||(a==CCU+0xa88&&!(value&BIT(31)))) {
        if(width!=4)return 0;
        for(unsigned i=0;i<sizeof(pll_ve0_consumers)/sizeof(pll_ve0_consumers[0]);++i) {
            uint32_t m;
            if(a==CCU+0xa88&&pll_ve0_consumers[i].module!=0xa88)continue;
            if(read(cookie,CCU+pll_ve0_consumers[i].module,&m)!=1)return -1;
            if((m&BIT(31))&&(a==CCU+0xa88||((m>>24)&7)==pll_ve0_consumers[i].mux))return 0;
        }
        mask=a==CCU+0x220?UINT32_C(0xf870ff02):UINT32_C(0x0700001f);
    }
    /* 0x5e4 admits only the key-value OR that opens bit 18 (open_mclk). The
     * decoder writes no other critical gate and no PCK600 register. */
    if(a==CCU+0x5e4&&(value&MCLK_KEY)==MCLK_KEY)mask=MCLK_KEY;
    int allowed=mask&&width==4;
    if(a>=VE&&a<VE+0x1000) {
        uint32_t off=(uint32_t)(a-VE);
        allowed=jpeg_write_allowed(off,width)||(width==4&&(h264_write_allowed(off)||hevc_write_allowed(off)||off==0||off==4||off==0x1c||off==0x50||off==0x54||off==0x804));
    }
    *owned=mask;
    return allowed;
}
static int native_policy_read(void *cookie,uintptr_t a,uint32_t *out)
{(void)cookie;return a733_video_unit_native_read(a,out);}
int a733_video_platform_native_write(void *cookie,uintptr_t a,unsigned width,uint32_t value)
{
    uint32_t mask;
    if(a733_video_platform_write_policy(a,width,value,native_policy_read,cookie,&mask)!=1)return -1;
    return a733_video_unit_native_write(a,width,mask,value);
}
