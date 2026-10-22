/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "a733_video_unit.h"
#include "a733_clock_reference.h"
#include "a733_range.h"
#define CCU UINT32_C(0x02002000)
#define R_CCU UINT32_C(0x07010000)
#define GIC UINT32_C(0x03400000)
#define BIT(n) (UINT32_C(1)<<(n))
static const uint64_t decoder_versions[]={
    UINT64_C(0x3332100021321),UINT64_C(0x3331000021320),UINT64_C(0x3052000012011)
};
static const uint64_t encoder_versions[]={UINT64_C(0x0003331000021320)};
/* Decoder: domain 2 and IRQ81; module mux 1 selects VE0. */
const struct a733_video_unit_desc a733_video_decoder_unit={
    .ve=UINT32_C(0x01c0e000),.pck=UINT32_C(0x07062000),.module=0xa88,.bus=BIT(18)|BIT(2),
    .mbus={BIT(0),BIT(14),BIT(18)},.autonomous={0x30001,0x20000},.irq=81,.ve0_mux=1,
    .exclusive=A733_CLOCK_VE_DEC,.identity=0,.feature={0,0x16670040},
    .versions=decoder_versions,.version_count=3
};
/* Encoder: domain 3 and IRQ80; module mux 0 selects VE0. Source: sun60iw2
 * platform encoder path and domain3 pck600 data. */
const struct a733_video_unit_desc a733_video_encoder_unit={
    .ve=UINT32_C(0x01c10000),.pck=UINT32_C(0x07063000),.module=0xa80,.bus=BIT(0)|BIT(16),
    .mbus={BIT(1),BIT(12),BIT(1)},.autonomous={0,0},.irq=80,.ve0_mux=0,
    .exclusive=A733_CLOCK_VE_ENC,.identity=0x800,.feature={0,0},
    .versions=encoder_versions,.version_count=1
};
static const uint32_t fabric_off[]={0x500,0x580,0x588};
static const uint32_t fabric_mask[]={0x0300001f,0x8700001f,0x8700001f};
static const uint32_t mhz[3][8]={
    {24,0,0,600,0,0,0,0},{24,0,800,600,480,0,0,0},{24,600,0,480,400,0,0,0}
};
static int ve0(const struct a733_video_unit_desc *d,uint32_t module)
{return ((module>>24)&1)==d->ve0_mux;}
int a733_video_unit_scopes(const struct a733_video_unit_desc *d,const struct a733_video_unit_config *c,
    a733_clock_mask_t *out)
{
    if(!d||!c||!out||!c->clocks||!c->power||!c->owner||!c->handoff||!c->module_hz||
       c->module_hz>720000000u||(c->module&~0x8700001fu)||!(c->module&BIT(31))||
       ((c->module>>24)&7)>1)return 0;
    a733_clock_mask_t shared=A733_CLOCK_FABRIC|A733_CLOCK_REF|A733_CLOCK_OSC|
        (ve0(d,c->module)?A733_CLOCK_PLL_VE0:A733_CLOCK_PLL_VE1);
    for(unsigned i=0;i<3;++i) {
        unsigned rate=mhz[i][(c->fabric[i]>>24)&7],div=(c->fabric[i]&31)+1;
        if((c->fabric[i]&~fabric_mask[i])||(i&&!(c->fabric[i]&BIT(31)))||!rate||
           (rate*1000000u)%div||c->fabric_hz[i]!=rate*1000000u/div)return 0;
        if(rate!=24)shared|=A733_CLOCK_PERI0;
    }
    *out=shared;return 1;
}
int a733_video_unit_init(struct a733_video_unit *u,const struct a733_video_unit_desc *d,
    const struct a733_video_unit_config *c,const struct a733_video_unit_ops *o,void *cookie)
{
    a733_clock_mask_t shared;
    if(!u||u->initialized||!o||!o->now_us||!o->enter||!o->leave||!o->read||!o->write||
       !a733_video_unit_scopes(d,c,&shared)||
       !c->clock.id||c->clock.owner!=c->owner||c->clock.shared!=shared||
       c->clock.exclusive!=d->exclusive||c->power_client>=A733_GPU_RESOURCE_CLIENTS||
       !c->pck.token||c->pck.mask!=(A733_GPU_RESOURCE_OSC|A733_GPU_RESOURCE_PCK)||
       c->pck.mode!=A733_GPU_RESOURCE_STABLE)return 0;
    *u=(struct a733_video_unit){.desc=d,.config=*c,.ops=*o,.cookie=cookie,.shared=shared,.initialized=1};
    for(unsigned i=0;i<3;++i)u->selected_mhz[i]=mhz[i][(c->fabric[i]>>24)&7];
    return 1;
}
int a733_video_unit_fail(struct a733_video_unit *u) {u->failed=1;return -1;}
int a733_video_unit_now(struct a733_video_unit *u,uint64_t *out)
{
    uint64_t n;
    if(!u||!u->initialized||u->failed||!out||u->ops.now_us(u->cookie,&n)!=1||
       !a733_monotonic_advance(n,&u->last_now))
        return u?a733_video_unit_fail(u):-1;
    *out=n;return 1;
}
int a733_video_unit_timely(struct a733_video_unit *u)
{uint64_t n;return a733_video_unit_now(u,&n)==1&&n<u->deadline;}
/* A busy registry is effect-free: another task is inside its short critical
 * section. Retry for at most two scheduler ticks, so an equal-priority holder
 * on this core can finish after a time slice; then report 0 (not held). */
#define BUSY_RETRY_US 2000u
int a733_video_unit_held(struct a733_video_unit *u)
{
    uint64_t first=0,n;
    for(unsigned tries=0;;++tries) {
        enum a733_clock_result c=a733_clock_token_status(u->config.clocks,&u->config.clock);
        if(c!=A733_CLOCK_OK&&c!=A733_CLOCK_BUSY)return a733_video_unit_fail(u);
        if(c==A733_CLOCK_OK) {
            enum a733_gpu_resource_result g=a733_gpu_resources_status(u->config.power,u->config.power_client,&u->config.pck);
            if(g==A733_GPU_RESOURCE_OK)return 1;
            if(g!=A733_GPU_RESOURCE_BUSY)return a733_video_unit_fail(u);
        }
        if(a733_video_unit_now(u,&n)!=1)return -1;
        if(!tries)first=n;
        if(n>=u->deadline||n-first>=BUSY_RETRY_US||tries==65535)return 0;
    }
}
int a733_video_unit_lock(struct a733_video_unit *u)
{
    if(u->failed||!a733_video_unit_timely(u))return a733_video_unit_fail(u);
    int h=a733_video_unit_held(u);if(h!=1)return h;
    if(u->locked)return a733_video_unit_fail(u);
    int r=u->ops.enter(u->cookie,u->deadline);
    if(r==1)u->locked=1;
    if((r!=0&&r!=1)||!a733_video_unit_timely(u))return a733_video_unit_fail(u);
    return r;
}
int a733_video_unit_unlock(struct a733_video_unit *u)
{
    if(u->failed||!u->locked||!a733_video_unit_timely(u))return a733_video_unit_fail(u);
    if(u->ops.leave(u->cookie)!=1)return a733_video_unit_fail(u);
    u->locked=0;
    return a733_video_unit_timely(u)?1:a733_video_unit_fail(u);
}
int a733_video_unit_read(struct a733_video_unit *u,uintptr_t a,uint32_t *v)
{
    if(!u->locked||!a733_video_unit_timely(u)||u->ops.read(u->cookie,a,v)!=1||
       !a733_video_unit_timely(u))return a733_video_unit_fail(u);
    return 1;
}
int a733_video_unit_write(struct a733_video_unit *u,uintptr_t a,unsigned width,uint32_t v)
{
    if(!u->locked||!a733_video_unit_timely(u)||u->ops.write(u->cookie,a,width,v)!=1||
       !a733_video_unit_timely(u))return a733_video_unit_fail(u);
    return 1;
}
int a733_video_unit_check(struct a733_video_unit *u,uintptr_t a,uint32_t mask,uint32_t expected)
{uint32_t v;return a733_video_unit_read(u,a,&v)==1&&(v&mask)==expected;}
int a733_video_unit_update(struct a733_video_unit *u,uintptr_t a,uint32_t mask,uint32_t value)
{
    uint32_t old,actual;
    if(a733_video_unit_read(u,a,&old)!=1)return 0;
    value=(old&~mask)|(value&mask);
    return a733_video_unit_write(u,a,4,value)==1&&a733_video_unit_read(u,a,&actual)==1&&actual==value;
}
static int parents(struct a733_video_unit *u)
{
    uint32_t osc,ref,pll,pat,gate,status;
    /* The shared rule admits an enabled, stable DCXO whose PLL_REF output is
     * normalized to 24 MHz, including the 26 MHz board's N=96/M=104. */
    if(a733_video_unit_read(u,0x07090160,&osc)!=1||a733_video_unit_read(u,CCU,&ref)!=1||
       !a733_clock_ref24_valid(osc,ref))return 0;
    unsigned n,m,v0=ve0(u->desc,u->config.module),off=v0?0x220:0x240;
    if(a733_video_unit_read(u,CCU+off,&pll)!=1||(pll&0xf8000000u)!=0xf8000000u||
       a733_video_unit_read(u,CCU+off+8,&pat)!=1||(pat&BIT(31))||
       a733_video_unit_read(u,CCU+off+12,&pat)!=1||(pat&BIT(27))||
       a733_video_unit_read(u,CCU+0x1918,&gate)!=1)return 0;
    unsigned required=v0?(BIT(0)|BIT(16)):(BIT(1)|BIT(17));
    if((gate&required)!=required||a733_video_unit_read(u,CCU+0x1998,&status)!=1||
       (status&(required&0xffff0000u))!=(required&0xffff0000u))return 0;
    /* No-Auto, requested enable and effective output status are independent. */
    n=((pll>>8)&255)+1;m=(((pll>>20)&7)+1)*((pll&2)?2:1)*(1+(u->config.module&31));
    if(n<53||n>105||24000000u*n%m||24000000u*n/m!=u->config.module_hz)return 0;
    if(!(u->shared&A733_CLOCK_PERI0))return 1;
    if(a733_video_unit_read(u,CCU+0xa0,&pll)!=1||(pll&0xfe000000u)!=0xfe000000u||
       (pll&(BIT(24)|BIT(1)))||a733_video_unit_read(u,CCU+0xa8,&pat)!=1||(pat&BIT(31))||
       a733_video_unit_read(u,CCU+0xac,&pat)!=1||(pat&BIT(27))||
       a733_video_unit_read(u,CCU+0x1908,&gate)!=1||a733_video_unit_read(u,CCU+0x1988,&status)!=1)return 0;
    n=((pll>>8)&255)+1;if(n<53||n>105)return 0;
    for(unsigned i=0;i<3;++i) {
        unsigned div,mask,rate=u->selected_mhz[i];
        switch(rate) {
        case 24:continue;
        case 600:div=(((pll>>20)&7)+1)*2;mask=BIT(11)|BIT(9);break;
        case 400:div=(((pll>>20)&7)+1)*3;mask=BIT(11)|BIT(2)|BIT(1);break;
        case 800:div=((pll>>16)&7)+1;mask=BIT(10);break;
        case 480:div=((pll>>2)&7)+1;mask=BIT(8)|BIT(7);break;
        default:return 0;
        }
        mask|=mask<<16;
        if((gate&mask)!=mask||(status&(mask&0xffff0000u))!=(mask&0xffff0000u)||
           24u*n!=rate*div)return 0;
    }
    return 1;
}
int a733_video_unit_clocks(struct a733_video_unit *u)
{
    for(unsigned i=0;i<3;++i)
        if(!a733_video_unit_check(u,CCU+fabric_off[i],fabric_mask[i],u->config.fabric[i]))return 0;
    return parents(u)&&a733_video_unit_check(u,R_CCU+0x1ac,1,1);
}
int a733_video_unit_irq_quiet(struct a733_video_unit *u)
{
    unsigned irq=u->desc->irq;uint32_t word=4*(irq/32),bit=BIT(irq%32);
    return a733_video_unit_check(u,GIC,BIT(31),0)&&a733_video_unit_check(u,GIC+0x100+word,bit,0)&&
        a733_video_unit_check(u,GIC+0x200+word,bit,0)&&a733_video_unit_check(u,GIC+0x300+word,bit,0)&&
        a733_video_unit_check(u,GIC+0xc00+4*(irq/16),3u<<(2*(irq%16)),0);
}
int a733_video_unit_baseline(struct a733_video_unit *u)
{
    /* Inspect CCU/PCK before any potentially inaccessible VE read. No cold
     * reset release/power-on is inferred from zeros or from identity values. */
    const struct a733_video_unit_desc *d=u->desc;
    return a733_video_unit_clocks(u)&&a733_video_unit_check(u,d->pck,15,8)&&
        a733_video_unit_check(u,d->pck+8,15,8)&&
        (!d->autonomous[0]||a733_video_unit_check(u,CCU+0x1b10,d->autonomous[0],d->autonomous[1]))&&
        a733_video_unit_check(u,CCU+d->module,0x8700001f,u->config.module)&&
        a733_video_unit_check(u,CCU+0xa8c,d->bus,d->bus)&&
        a733_video_unit_check(u,CCU+0x5c0,d->mbus[0],d->mbus[0])&&
        a733_video_unit_check(u,CCU+0x5e0,d->mbus[1],d->mbus[1])&&
        a733_video_unit_check(u,CCU+0x5e4,d->mbus[2],d->mbus[2])&&a733_video_unit_irq_quiet(u);
}
int a733_video_unit_identity(struct a733_video_unit *u,uint64_t *version)
{
    const struct a733_video_unit_desc *d=u->desc;uintptr_t base=d->ve+d->identity;uint32_t f,h,l;
    if(a733_video_unit_read(u,base+0xf0,&f)!=1||(f!=d->feature[0]&&f!=d->feature[1])||
       a733_video_unit_read(u,base+0xe0,&h)!=1||a733_video_unit_read(u,base+0xe4,&l)!=1)return 0;
    *version=((uint64_t)(h&0xfffff)<<32)|(l&0xfffff);
    for(unsigned i=0;i<d->version_count;++i)if(*version==d->versions[i])return 1;
    return 0;
}
#ifdef __aarch64__
#include "a733_mapping_check.h"
#include "a733_mmio.h"
static int device(uintptr_t a)
{
    static const struct a733_mapping_rule rule={A733_MAPPING_WRITE,1,{{A733_MAIR_DEVICE_NGNRNE,A733_SH_OUTER}},0,1};
    return a733_mapping_check(aarch64_device_translate,a,a,&rule);
}
#endif
int a733_video_unit_native_read(uintptr_t a,uint32_t *out)
{
#ifdef __aarch64__
    if(!out||(a&3)||!device(a))return -1;
    *out=a733_mmio_read32_dsb_sy_both(a);return 1;
#else
    (void)a;(void)out;return -1;
#endif
}
int a733_video_unit_native_write(uintptr_t a,unsigned width,uint32_t owned,uint32_t value)
{
#ifdef __aarch64__
    if((width!=1&&width!=4)||(a&(width-1))||(owned&&width!=4)||!device(a))return -1;
    a733_mmio_dsb_sy();
    if(owned&&((a733_mmio_read32(a)^value)&~owned))return -1;
    if(width==1)a733_mmio_write8(a,(uint8_t)value);
    else a733_mmio_write32(a,value);
    a733_mmio_dsb_sy();return 1;
#else
    (void)a;(void)width;(void)owned;(void)value;return -1;
#endif
}
