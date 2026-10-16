/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "a733_video_platform.h"
#define CCU UINT32_C(0x02002000)
#define PCK UINT32_C(0x07062000)
#define R_CCU UINT32_C(0x07010000)
#define VE UINT32_C(0x01c0e000)
#define GIC UINT32_C(0x03400000)
#define BIT(n) (UINT32_C(1)<<(n))
static const uint32_t fabric_off[]={0x500,0x580,0x588};
static const uint32_t fabric_mask[]={0x0300001f,0x8700001f,0x8700001f};
static const uint32_t mhz[3][8]={
    {24,0,0,600,0,0,0,0},{24,0,800,600,480,0,0,0},{24,600,0,480,400,0,0,0}
};
static int known(uint64_t v)
{return v==UINT64_C(0x3332100021321)||v==UINT64_C(0x3331000021320)||v==UINT64_C(0x3052000012011);}
int a733_video_platform_scopes(const struct a733_video_platform_config *c,uint32_t *out)
{
    if(!c||!out||!c->clocks||!c->power||!c->owner||!c->handoff||!c->module_hz||
       (c->start!=A733_VIDEO_INHERITED&&c->start!=A733_VIDEO_COLD_NS_ROM)||
       c->module_hz>720000000u||(c->module&~0x8700001fu)||!(c->module&BIT(31))||
       ((c->module>>24)&7)>1)return 0;
    uint32_t shared=A733_CLOCK_FABRIC|A733_CLOCK_REF|A733_CLOCK_OSC|
        ((c->module&BIT(24))?A733_CLOCK_PLL_VE0:A733_CLOCK_PLL_VE1);
    for(unsigned i=0;i<3;++i) {
        unsigned rate=mhz[i][(c->fabric[i]>>24)&7],div=(c->fabric[i]&31)+1;
        if((c->fabric[i]&~fabric_mask[i])||(i&&!(c->fabric[i]&BIT(31)))||!rate||
           (rate*1000000u)%div||c->fabric_hz[i]!=rate*1000000u/div)return 0;
        if(rate!=24)shared|=A733_CLOCK_PERI0;
    }
    *out=shared;return 1;
}
int a733_video_platform_init(struct a733_video_platform *p,
 const struct a733_video_platform_config *c,const struct a733_video_platform_ops *o,void *cookie)
{
    uint32_t shared;
    if(!p||p->initialized||!o||!o->now_us||!o->admit||!o->memory||!o->drain||
       !o->enter||!o->leave||!o->read||!o->write||!a733_video_platform_scopes(c,&shared)||
       !c->clock.id||c->clock.owner!=c->owner||c->clock.shared!=shared||
       c->clock.exclusive!=A733_CLOCK_VE_DEC||c->power_client>=A733_GPU_RESOURCE_CLIENTS||
       !c->pck.token||c->pck.mask!=(A733_GPU_RESOURCE_OSC|A733_GPU_RESOURCE_PCK)||
       c->pck.mode!=A733_GPU_RESOURCE_STABLE)return 0;
    const struct a733_video_platform_config config=*c;
    const struct a733_video_platform_ops ops=*o;
    *p=(struct a733_video_platform){.config=config,.ops=ops,.cookie=cookie,.shared=shared,.initialized=1};
    for(unsigned i=0;i<3;++i)p->selected_mhz[i]=mhz[i][(config.fabric[i]>>24)&7];
    return 1;
}
static int fail(struct a733_video_platform *p) {p->failed=1;return -1;}
static int clock_now(void *v,uint64_t *out)
{
    struct a733_video_platform *p=v;uint64_t n;
    if(!p||!p->initialized||p->failed||!out||p->ops.now_us(p->cookie,&n)!=1||n<p->last_now)
        return p?fail(p):-1;
    p->last_now=n;*out=n;return 1;
}
static int timely(struct a733_video_platform *p)
{uint64_t n;return clock_now(p,&n)==1&&n<p->deadline;}
static int held(struct a733_video_platform *p)
{
    enum a733_clock_result c=a733_clock_token_status(p->config.clocks,&p->config.clock);
    if(c==A733_CLOCK_BUSY)return 0;
    if(c!=A733_CLOCK_OK)return fail(p);
    enum a733_gpu_resource_result g=a733_gpu_resources_status(p->config.power,p->config.power_client,&p->config.pck);
    if(g==A733_GPU_RESOURCE_BUSY)return 0;
    return g==A733_GPU_RESOURCE_OK?1:fail(p);
}
static int lock(struct a733_video_platform *p)
{
    if(p->failed||!timely(p))return fail(p);
    int h=held(p);if(h!=1)return h;
    if(p->locked)return fail(p);
    int r=p->ops.enter(p->cookie,p->deadline);
    if(r==1)p->locked=1;
    if((r!=0&&r!=1)||!timely(p))return fail(p);
    return r;
}
static int unlock(struct a733_video_platform *p)
{
    if(p->failed||!p->locked||!timely(p))return fail(p);
    if(p->ops.leave(p->cookie)!=1)return fail(p);
    p->locked=0;
    return timely(p)?1:fail(p);
}
static int rd(struct a733_video_platform *p,uintptr_t a,uint32_t *v)
{
    if(!p->locked||!timely(p)||p->ops.read(p->cookie,a,v)!=1||!timely(p))return fail(p);
    return 1;
}
static int wr(struct a733_video_platform *p,uintptr_t a,unsigned width,uint32_t v)
{
    if(!p->locked||!timely(p)||p->ops.write(p->cookie,a,width,v)!=1||!timely(p))return fail(p);
    return 1;
}
static int check(struct a733_video_platform *p,uintptr_t a,uint32_t mask,uint32_t expected)
{uint32_t v;return rd(p,a,&v)==1&&(v&mask)==expected;}
static int update(struct a733_video_platform *p,uintptr_t a,uint32_t mask,uint32_t value)
{
    uint32_t old,actual;
    if(rd(p,a,&old)!=1)return 0;
    value=(old&~mask)|(value&mask);
    return wr(p,a,4,value)==1&&rd(p,a,&actual)==1&&actual==value;
}
static int parents(struct a733_video_platform *p)
{
    uint32_t osc,ref,pll,pat,gate,status;
    if(rd(p,0x07090160,&osc)!=1||(((osc>>14)&3)!=0&&((osc>>14)&3)!=3)||
       rd(p,CCU,&ref)!=1||(ref&0xf8000000u)!=0xf8000000u)return 0;
    unsigned n=((ref>>8)&255)+1,m=((ref>>16)&127)+1;
    if(n<80||n>116||n!=m)return 0;
    unsigned v0=!!(p->config.module&BIT(24)),off=v0?0x220:0x240;
    if(rd(p,CCU+off,&pll)!=1||(pll&0xf8000000u)!=0xf8000000u||
       rd(p,CCU+off+8,&pat)!=1||(pat&BIT(31))||
       rd(p,CCU+off+12,&pat)!=1||(pat&BIT(27))||
       rd(p,CCU+0x1918,&gate)!=1)return 0;
    unsigned required=v0?(BIT(0)|BIT(16)):(BIT(1)|BIT(17));
    if((gate&required)!=required||rd(p,CCU+0x1998,&status)!=1||
       (status&(required&0xffff0000u))!=(required&0xffff0000u))return 0;
    /* No-Auto, requested enable and effective output status are independent. */
    n=((pll>>8)&255)+1;m=(((pll>>20)&7)+1)*((pll&2)?2:1)*(1+(p->config.module&31));
    if(n<53||n>105||24000000u*n%m||24000000u*n/m!=p->config.module_hz)return 0;
    if(!(p->shared&A733_CLOCK_PERI0))return 1;
    if(rd(p,CCU+0xa0,&pll)!=1||(pll&0xfe000000u)!=0xfe000000u||
       (pll&(BIT(24)|BIT(1)))||rd(p,CCU+0xa8,&pat)!=1||(pat&BIT(31))||
       rd(p,CCU+0xac,&pat)!=1||(pat&BIT(27))||rd(p,CCU+0x1908,&gate)!=1||rd(p,CCU+0x1988,&status)!=1)return 0;
    n=((pll>>8)&255)+1;if(n<53||n>105)return 0;
    for(unsigned i=0;i<3;++i) {
        unsigned div,mask,rate=p->selected_mhz[i];
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
static int baseline(struct a733_video_platform *p)
{
    /* Inspect CCU/PCK before any potentially inaccessible VE read. No cold
     * reset release/power-on is inferred from zeros or from identity values. */
    for(unsigned i=0;i<3;++i)
        if(!check(p,CCU+fabric_off[i],fabric_mask[i],p->config.fabric[i]))return 0;
    return parents(p)&&check(p,R_CCU+0x1ac,1,1)&&check(p,PCK,15,8)&&check(p,PCK+8,15,8)&&
        check(p,CCU+0x1b10,0x30001,0x20000)&&
        check(p,CCU+0xa88,0x8700001f,p->config.module)&&check(p,CCU+0xa8c,BIT(18)|BIT(2),BIT(18)|BIT(2))&&
        check(p,CCU+0x5c0,1,1)&&check(p,CCU+0x5e0,BIT(14),BIT(14))&&
        check(p,CCU+0x5e4,BIT(18),BIT(18))&&
        check(p,GIC,BIT(31),0)&&check(p,GIC+0x108,BIT(17),0)&& /* SPI81 masked. */
        check(p,GIC+0x208,BIT(17),0)&&check(p,GIC+0x308,BIT(17),0)&&
        check(p,GIC+0xc14,3u<<2,0); /* IRQ81 level, no pending/active state. */
}
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
static int cold_entry(struct a733_video_platform *p)
{
    for(unsigned i=0;i<sizeof(rom_identity)/sizeof(rom_identity[0]);++i)
        if(!check(p,rom_identity[i].address,UINT32_MAX,rom_identity[i].word))return 0;
    for(unsigned i=0;i<3;++i)
        if(!check(p,CCU+fabric_off[i],fabric_mask[i],p->config.fabric[i]))return 0;
    return parents(p)&&check(p,R_CCU+0x1ac,1,1)&&
        check(p,PCK,15,0)&&check(p,PCK+8,15,0)&&
        check(p,CCU+0xa88,0x8700001f,p->config.module&~BIT(31))&&
        check(p,CCU+0xa8c,BIT(18)|BIT(2),0)&&
        check(p,CCU+0x5c0,1,0)&&check(p,CCU+0x5e0,BIT(14),0)&&
        check(p,CCU+0x5e4,BIT(18),0)&&check(p,CCU+0x1b10,0x30001,0x10000)&&
        check(p,GIC,BIT(31),0)&&check(p,GIC+0x108,BIT(17),0)&&
        check(p,GIC+0x208,BIT(17),0)&&check(p,GIC+0x308,BIT(17),0)&&
        check(p,GIC+0xc14,3u<<2,0);
}
/* One bounded action per acquire call. Reset remains asserted through the PCK
 * ON acknowledgement. Preserve all unrelated gates: the BSP's key-OR aliases
 * conflict with ordinary gate fields documented in manual4.1.6.82-84. */
static int cold(struct a733_video_platform *p)
{
    switch(p->cold_step) {
    case 0:return cold_entry(p);
    case 1:return update(p,PCK+0x170,UINT32_MAX,0x1f1f1f);
    case 2:return update(p,PCK+0x174,UINT32_MAX,0x1f1f);
    case 3:return update(p,PCK+0xc00,UINT32_MAX,0x08080808);
    case 4:return update(p,PCK+0xc04,UINT32_MAX,0x0808);
    case 5:return update(p,PCK+0xc10,UINT32_MAX,8);
    case 6:return update(p,PCK,15,8);
    case 7: {
        uint32_t status;
        if(rd(p,PCK+8,&status)!=1)return 0;
        return (status&15)==8?1:2; /* Bounded by the common deadline. */
    }
    case 8:return update(p,CCU+0xa8c,BIT(18),BIT(18));
    case 9:return update(p,CCU+0x5c0,1,1);
    case 10:return update(p,CCU+0x5e0,BIT(14),BIT(14));
    case 11:return update(p,CCU+0x5e4,BIT(18),BIT(18));
    case 12:return update(p,CCU+0xa8c,BIT(2),BIT(2));
    case 13:return update(p,CCU+0xa88,BIT(31),BIT(31));
    case 14:return baseline(p);
    /* A733 kernel cold startup always pulses VCU before any compound-ID
     * lookup. Immutable ROM/resource admission authorizes only this reset. */
    case 15:return update(p,VE+0x804,0x11,0x11);
    case 16:return update(p,VE+0x804,0x11,0);
    default:return 0;
    }
}
static int acquire(void *v,const struct a733_video_jpeg_job *j,uint64_t d,uint64_t *token)
{
    struct a733_video_platform *p=v;
    if(!p||!p->initialized||p->failed||!token||!d||(p->deadline&&p->deadline!=d))return -1;
    p->deadline=d;
    if(!timely(p))return fail(p);
    if(!p->admitted) {
        uint64_t previous=p->token;
        int r=p->ops.admit(p->cookie,&p->config,j,d,&p->token);
        *token=p->token;
        if(!p->token||(previous&&previous!=p->token)||(r!=0&&r!=1)||!timely(p))return fail(p);
        if(!r)return 0;
        p->admitted=1;
    }
    *token=p->token;
    if(p->checked)return 1;
    int r=lock(p);if(r!=1)return r;
    if(p->config.start==A733_VIDEO_COLD_NS_ROM) {
        int c=cold(p);
        if(!c)return fail(p);
        if(c==1)++p->cold_step;
        if(unlock(p)!=1)return -1;
        if(p->cold_step<17)return 0;
    }else {
        if(!baseline(p))return fail(p);
        if(unlock(p)!=1)return -1;
    }
    p->checked=1;return 1;
}
static int identity(struct a733_video_platform *p)
{
    uint32_t f,h,l;
    if(rd(p,VE+0xf0,&f)!=1||(f!=0&&f!=0x16670040)||rd(p,VE+0xe0,&h)!=1||rd(p,VE+0xe4,&l)!=1)return 0;
    p->version=((uint64_t)(h&0xfffff)<<32)|(l&0xfffff);
    return known(p->version);
}
/* VeReset policy45/52 delegates external reset; policy51 uses top+4 bit16.
 * Remaining updates combine package RMW operations with the same final fields.
 * A reset pulse is NOT a drain mechanism: authority must already prove drain.
 */
static int prep_step(struct a733_video_platform *p)
{
    unsigned local=p->version==UINT64_C(0x3331000021320);
    switch(p->step) {
    case 0:return update(p,VE,15,7);
    case 1:
        if(!update(p,local?VE+4:CCU+0xa8c,local?BIT(16):BIT(18),local?BIT(16):0))return 0;
        if(!local) {if(p->last_now>UINT64_MAX-10)return 0;p->reset_until=p->last_now+10;}
        return 1;
    case 2:return update(p,local?VE+4:CCU+0xa8c,local?BIT(16):BIT(18),local?0:BIT(18));
    case 3:return local?1:update(p,VE+0x804,0x11,0x11);
    case 4:return local?1:update(p,VE+0x804,0x11,0);
    case 5:return local?1:update(p,VE,0x130000,0x130000);
    case 6:return update(p,VE,0x2c00101f,0);
    case 7:return update(p,VE+0x1c,0x100,0);
    case 8:return update(p,VE+0xc4,UINT32_MAX,0);
    case 9:return update(p,VE+0xc8,UINT32_MAX,0);
    case 10:return update(p,VE+0xcc,UINT32_MAX,0);
    case 11:return update(p,VE+0xe8,UINT32_MAX,0);
    case 12:return update(p,VE+0xec,0x30077,0);
    case 13:return update(p,VE+0x50,15,0);
    case 14:return update(p,VE+0x54,UINT32_MAX,0);
    case 15:return p->version==UINT64_C(0x3052000012011)?1:update(p,VE,0x400,0x400);
    case 16:return update(p,VE,0x600000,0); /* width <=1920 */
    case 17:return update(p,VE,0x130000,0x130000); /* Package DRAM mode5. */
    default:return 0;
    }
}
static int prepare(void *v,uint64_t token,const struct a733_video_jpeg_job *j,
 const struct a733_video_jpeg *pic,uint32_t stride,uint32_t rows,uint64_t d)
{
    struct a733_video_platform *p=v;
    if(p->failed||!p->checked||token!=p->token||d!=p->deadline||!timely(p))return fail(p);
    if(p->prepared)return 1;
    if(!p->version) {
        int r=lock(p);if(r!=1)return r;
        if(!identity(p))return fail(p);
        if(unlock(p)!=1)return -1;
        return 0;
    }
    if(!p->mapped) {
        int h=held(p);if(h!=1)return h;
        int r=p->ops.memory(p->cookie,token,j,pic,stride,rows,d);
        if((r!=0&&r!=1)||!timely(p))return fail(p);
        if(r==1)p->mapped=1;
        return 0;
    }
    if(p->step==2&&p->last_now<p->reset_until)return 0;
    int r=lock(p);if(r!=1)return r;
    if(!prep_step(p))return fail(p);
    ++p->step;
    if(unlock(p)!=1)return -1;
    if(p->step==18)p->prepared=1;
    return p->prepared?1:0;
}
static int jpeg_read(void *v,uint64_t token,uint32_t off,uint32_t *out,uint64_t d)
{
    struct a733_video_platform *p=v;
    if(p->failed||!p->checked||token!=p->token||d!=p->deadline||!out)return fail(p);
    if(off!=0xf0&&off!=0xe0&&off!=0xe4&&(!p->prepared||(off!=0&&off!=0x11c&&off!=0xec)))return fail(p);
    if(lock(p)!=1)return fail(p);
    if(rd(p,VE+off,out)!=1)return -1;
    return unlock(p);
}
static int codec_write(uint32_t off,unsigned width)
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
    if(p->failed||!p->prepared||token!=p->token||d!=p->deadline||!codec_write(off,width))return fail(p);
    if(lock(p)!=1)return fail(p);
    if(wr(p,VE+off,width,value)!=1)return -1;
    return unlock(p);
}
static int finish(void *v,uint64_t token,uint64_t d)
{
    struct a733_video_platform *p=v;
    if(p->failed||!p->prepared||token!=p->token||d!=p->deadline||!timely(p))return fail(p);
    int h=held(p);if(h!=1)return h;
    int r=p->ops.drain(p->cookie,token,d);
    return (r==0||r==1)&&timely(p)?r:fail(p);
}
const struct a733_video_jpeg_ops *a733_video_platform_jpeg_ops(void)
{
    static const struct a733_video_jpeg_ops ops={clock_now,acquire,prepare,jpeg_read,jpeg_write,finish};
    return &ops;
}
static int readable(uintptr_t a)
{
    for(unsigned i=0;i<sizeof(rom_identity)/sizeof(rom_identity[0]);++i)
        if(a==rom_identity[i].address)return 1;
    if(a==0x07090160||a==R_CCU+0x1ac||a==PCK||a==PCK+8||
       a==PCK+0x170||a==PCK+0x174||a==PCK+0xc00||a==PCK+0xc04||a==PCK+0xc10)return 1;
    if(a>=CCU&&a<CCU+0x2000) switch(a-CCU) {
    case 0:case 0xa0:case 0xa8:case 0xac:case 0x220:case 0x228:case 0x22c:
    case 0x240:case 0x248:case 0x24c:case 0x500:case 0x580:case 0x588:
    case 0x5c0:case 0x5e0:case 0x5e4:case 0xa88:case 0xa8c:case 0x1908:case 0x1918:case 0x1988:case 0x1998:case 0x1b10:return 1;
    default:return 0;
    }
    if(a==GIC||a==GIC+0x108||a==GIC+0x208||a==GIC+0x308||a==GIC+0xc14)return 1;
    if(a>=VE&&a<VE+0x1000) switch(a-VE) {
    case 0:case 4:case 0x1c:case 0x50:case 0x54:case 0xc4:case 0xc8:case 0xcc:
    case 0xe0:case 0xe4:case 0xe8:case 0xec:case 0xf0:case 0x11c:case 0x804:return 1;
    default:return 0;
    }
    return 0;
}
int a733_video_platform_native_read(void *cookie,uintptr_t a,uint32_t *out)
{
    (void)cookie;
    if(!out||!readable(a))return -1;
#if defined(__aarch64__)
    __asm__ volatile("dsb sy":::"memory");
    *out=*(volatile const uint32_t *)a;
    __asm__ volatile("dsb sy":::"memory");return 1;
#else
    return -1;
#endif
}
int a733_video_platform_native_write(void *cookie,uintptr_t a,unsigned width,uint32_t value)
{
    (void)cookie;
    uint32_t mask=0;
    if(a==CCU+0xa8c)mask=BIT(18)|BIT(2);
    if(a==CCU+0xa88)mask=BIT(31);
    if(a==CCU+0x5c0)mask=1;
    if(a==CCU+0x5e0)mask=BIT(14);
    if(a==CCU+0x5e4)mask=BIT(18);
    if(a==PCK)mask=15;
    int delay=(a==PCK+0x170&&value==0x1f1f1f)||(a==PCK+0x174&&value==0x1f1f)||
        (a==PCK+0xc00&&value==0x08080808)||(a==PCK+0xc04&&value==0x0808)||(a==PCK+0xc10&&value==8);
    int allowed=(mask||delay)&&width==4;
    if(a>=VE&&a<VE+0x1000) {
        uint32_t off=(uint32_t)(a-VE);
        allowed=codec_write(off,width)||(width==4&&(off==0||off==4||off==0x1c||off==0x50||off==0x54||off==0x804));
    }
    if(!allowed)return -1;
#if defined(__aarch64__)
    __asm__ volatile("dsb sy":::"memory");
    if(mask&&((*(volatile const uint32_t *)a^value)&~mask))return -1;
    if(width==1)*(volatile uint8_t *)a=(uint8_t)value;
    else *(volatile uint32_t *)a=value;
    __asm__ volatile("dsb sy":::"memory");return 1;
#else
    (void)value;return -1;
#endif
}
