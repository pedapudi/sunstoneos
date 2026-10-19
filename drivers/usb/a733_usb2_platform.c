/* SPDX-License-Identifier: GPL-2.0-only
 * DWC3 global-clock/PHY programming follows drivers/usb/dwc3/core.c:
 * Copyright (C) 2010-2011 Texas Instruments Incorporated - https://www.ti.com
 * Authors: Felipe Balbi <balbi@ti.com>,
 *          Sebastian Andrzej Siewior <bigeasy@linutronix.de>
 */
#include "a733_usb2_platform.h"
#include "a733_usb3_phy.h"
#define B(n) (UINT32_C(1)<<(n))
#define CCU UINT32_C(0x02002000)
#define DWC UINT32_C(0x06a00000)
#define PHY UINT32_C(0x06b00000)
enum { CLOCK,RAIL,RAIL_WAIT,ADMIT,PCK,SUPPLY,ATTACH,BASELINE,CLOCKS,PHY_SETUP,SS_SETUP,GLOBAL,READY };
static const enum a733_rail supplies[A733_USB2_SUPPLIES+1]={A733_RAIL_DCDC1,A733_RAIL_DC1SW2,
    A733_RAIL_DCDC9,A733_RAIL_ELDO6,A733_RAIL_CLDO5,A733_RAIL_DCDC2};
static const uint32_t fabric_off[]={0x500,0x580,0x588};
static const uint32_t fabric_mask[]={0x0300001f,0x8700001f,0x8700001f};
static const unsigned parent_mhz[3][8]={{24,0,0,600},{24,0,800,600,480},{24,600,0,480,400}};
static int separate(const void *a,size_t an,const void *b,size_t bn)
{uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;return a&&b&&an<=UINTPTR_MAX-x&&bn<=UINTPTR_MAX-y&&(x+an<=y||y+bn<=x);}
int a733_usb2_platform_outside(struct a733_usb2_platform *p,const void *v,size_t n)
{
    return p&&p->self==p&&separate(p,sizeof(*p),v,n)&&
        separate(p->clocks,sizeof(*p->clocks),v,n)&&separate(p->rails,sizeof(*p->rails),v,n)&&
        separate(p->pmic,sizeof(*p->pmic),v,n)&&separate(p->pck,sizeof(*p->pck),v,n)&&
        separate(p->typec,sizeof(*p->typec),v,n)&&p->ops.outside(p->cookie,v,n)==1&&
        p->pmic->ops.outside(p->pmic->cookie,v,n)==1&&
        p->typec->config.ops.outside(p->typec->config.context,v,n)==1;
}
static int valid(const struct a733_usb2_platform_config *c)
{
    if(!c->owner||!c->identity||!c->topology||!c->deadline||c->lifetime_deadline<c->deadline||
        c->superspeed>1||c->pck_client>=A733_GPU_RESOURCE_CLIENTS||c->supply_uv[0]!=c->supply_uv[1])return 0;
    for(unsigned i=0;i<A733_USB2_SUPPLIES;++i)if(!c->supply_uv[i])return 0;
    switch(c->revision){case 0x3131302a:case 0x3132302a:case 0x3136302a:case 0x3137302a:
        case 0x3138302a:case 0x3139302a:case 0x3230302a:break;default:return 0;}
    for(unsigned i=0;i<3;++i){unsigned m=parent_mhz[i][(c->fabric[i]>>24)&7],d=(c->fabric[i]&31)+1;
        if((c->fabric[i]&~fabric_mask[i])||(i&&!(c->fabric[i]&B(31)))||!m||
            m*1000000u%d||c->fabric_hz[i]!=m*1000000u/d)return 0;}
    return 1;
}
int a733_usb2_platform_init(struct a733_usb2_platform *p,const struct a733_usb2_platform_config *c,
    const struct a733_usb2_platform_ops *o,void *cookie,struct a733_clock_leases *clocks,
    struct a733_rail_leases *rails,struct a733_pmic *pmic,struct a733_gpu_resource_manager *pck,
    struct a733_typec *typec)
{
    if(!p||p->self||!c||!o||!clocks||!rails||!pmic||!pck||!typec||
        !clocks->initialized||rails->self!=rails||pmic->self!=pmic||!pck->initialized||
        typec->self!=typec||rails->binding.gpu!=pck||rails->binding.clocks!=clocks||
        pmic->clocks!=clocks||pmic->rails!=rails||!valid(c)||!o->now||!o->current||
        (c->superspeed&&(!o->read16||!o->write16||typec->config.source_ma<900))||
        !o->admit||!o->enter||!o->leave||!o->retain||!o->read||!o->write||!o->outside||
        !separate(p,sizeof(*p),c,sizeof(*c))||!separate(p,sizeof(*p),o,sizeof(*o))||
        !separate(p,sizeof(*p),clocks,sizeof(*clocks))||!separate(p,sizeof(*p),rails,sizeof(*rails))||
        !separate(p,sizeof(*p),pmic,sizeof(*pmic))||!separate(p,sizeof(*p),pck,sizeof(*pck))||
        !separate(p,sizeof(*p),typec,sizeof(*typec))||o->outside(cookie,p,sizeof(*p))!=1||
        pmic->ops.outside(pmic->cookie,p,sizeof(*p))!=1||
        typec->config.ops.outside(typec->config.context,p,sizeof(*p))!=1)return 0;
    *p=(struct a733_usb2_platform){.self=p,.config=*c,.ops=*o,.cookie=cookie,.clocks=clocks,
        .rails=rails,.pmic=pmic,.pck=pck,.typec=typec,.deadline=c->deadline};
    atomic_init(&p->busy,0);return 1;
}
static int timely(struct a733_usb2_platform *p)
{uint64_t n;if(p->ops.now(p->cookie,&n)!=1||n<p->last_now)return 0;p->last_now=n;return n<p->deadline&&n<p->config.lifetime_deadline;}
static int health(struct a733_usb2_platform *);
static int attachment(struct a733_usb2_platform *);
static int permitted(struct a733_usb2_platform *p)
{return timely(p)&&health(p)==1&&attachment(p)==1&&timely(p);}
static int rd(struct a733_usb2_platform *p,uintptr_t a,uint32_t *v)
{return permitted(p)&&p->ops.read(p->cookie,a,v)==1&&permitted(p);}
static int ck(struct a733_usb2_platform *p,uintptr_t a,uint32_t m,uint32_t v)
{uint32_t r;return rd(p,a,&r)&&(r&m)==v;}
static int update(struct a733_usb2_platform *p,uintptr_t a,uint32_t m,uint32_t v)
{
    uint32_t r,got;if(!rd(p,a,&r))return 0;uint32_t next=(r&~m)|v;
    return (next==r||(permitted(p)&&p->ops.write(p->cookie,a,next)==1&&permitted(p)))&&rd(p,a,&got)&&got==next;
}
int a733_usb3_read32(struct a733_usb2_platform *p,uintptr_t a,uint32_t *v)
{return p->config.superspeed&&p->held&&!p->ss_failed&&permitted(p)&&p->ops.read(p->cookie,a,v)==1&&permitted(p);}
int a733_usb3_write32(struct a733_usb2_platform *p,uintptr_t a,uint32_t v)
{return p->config.superspeed&&p->held&&!p->ss_failed&&permitted(p)&&p->ops.write(p->cookie,a,v)==1&&permitted(p);}
int a733_usb3_read16(struct a733_usb2_platform *p,uintptr_t a,uint16_t *v)
{return p->config.superspeed&&p->held&&!p->ss_failed&&a733_usb3_address16(a)&&permitted(p)&&p->ops.read16(p->cookie,a,v)==1&&permitted(p);}
int a733_usb3_write16(struct a733_usb2_platform *p,uintptr_t a,uint16_t v)
{return p->config.superspeed&&p->held&&!p->ss_failed&&a733_usb3_address16(a)&&permitted(p)&&p->ops.write16(p->cookie,a,v)==1&&permitted(p);}
static int health(struct a733_usb2_platform *p)
{
    if(p->clock.id){enum a733_clock_result r=a733_clock_token_status(p->clocks,&p->clock);
        if(r==A733_CLOCK_BUSY)return 0;if(r!=A733_CLOCK_OK)return -1;}
    if(p->phase>RAIL_WAIT){enum a733_rail_result r=a733_rail_status(p->rails,&p->rail);
        if(r==A733_RAIL_BUSY)return 0;if(r!=A733_RAIL_READY)return -1;}
    if(p->pck_lease.token){enum a733_gpu_resource_result r=a733_gpu_resources_status(p->pck,p->config.pck_client,&p->pck_lease);
        if(r==A733_GPU_RESOURCE_BUSY)return 0;if(r!=A733_GPU_RESOURCE_OK)return -1;}
    if(p->phase>SUPPLY){enum a733_pmic_status r=a733_pmic_live(p->pmic,&p->pmic_lifetime);
        if(r==A733_PMIC_BUSY)return 0;if(r!=A733_PMIC_READY)return -1;}
    return 1;
}
static int attachment(struct a733_usb2_platform *p)
{
    struct a733_typec_token t;int r=a733_typec_snapshot(p->typec,&t,p->deadline);
    if(r!=1)return r;
    if(p->attachment.generation&&(t.provider!=p->attachment.provider||t.owner!=p->attachment.owner||
        t.generation!=p->attachment.generation||t.polarity!=p->attachment.polarity))return -1;
    r=a733_typec_validate(p->typec,&t,p->deadline);if(r==1)p->attachment=t;return r;
}
static enum a733_usb2_result fault(struct a733_usb2_platform *p)
{
    p->failed=1;a733_typec_revoke(p->typec);
    if(p->held){p->ops.retain(p->cookie);p->held=0;}
    if(p->clock.id&&!p->clock_retained&&a733_clock_retain(p->clocks,&p->clock)!=A733_CLOCK_BUSY)p->clock_retained=1;
    if(p->rail.id&&!p->rail_retained&&a733_rail_retain(p->rails,&p->rail)!=A733_RAIL_BUSY)p->rail_retained=1;
    return A733_USB2_RETAINED;
}
static enum a733_usb2_result finish(struct a733_usb2_platform *p,enum a733_usb2_result r)
{if(p->held){p->ops.leave(p->cookie);p->held=0;}atomic_store_explicit(&p->busy,0,memory_order_release);return r;}
static int enter(struct a733_usb2_platform *p)
{if(!p||p->self!=p||p->ops.current(p->cookie)!=1)return 0;unsigned z=0;return atomic_compare_exchange_strong_explicit(&p->busy,&z,1,memory_order_acquire,memory_order_relaxed);}
static int parents(struct a733_usb2_platform *p)
{
    uint32_t osc,ref,pll,a,b,g,s;
    if(!rd(p,0x07090160,&osc)||(osc&(B(28)|B(1)))!=(B(28)|B(1))||
        (((osc>>14)&3)!=0&&((osc>>14)&3)!=3)||!rd(p,CCU,&ref)||(ref&0xf8000000u)!=0xf8000000u)return 0;
    unsigned n=((ref>>8)&255)+1,m=((ref>>16)&127)+1;if(n<80||n>116||n!=m)return 0;
    for(unsigned i=0;i<3;++i)if(!ck(p,CCU+fabric_off[i],fabric_mask[i],p->config.fabric[i]))return 0;
    if(!rd(p,CCU+0xa0,&pll)||(pll&0xfe000000u)!=0xfe000000u||(pll&(B(24)|B(1)))||
        !rd(p,CCU+0xa8,&a)||(a&B(31))||!rd(p,CCU+0xac,&b)||(b&B(27))||
        !rd(p,CCU+0x1908,&g)||!rd(p,CCU+0x1988,&s))return 0;
    n=((pll>>8)&255)+1;if(n<53||n>105)return 0;
    for(unsigned i=0;i<5;++i){unsigned mhz=i<3?parent_mhz[i][(p->config.fabric[i]>>24)&7]:i==3?480:600;
        unsigned d,branches;switch(mhz){case 24:continue;case 600:d=(((pll>>20)&7)+1)*2;branches=B(11)|B(9);break;
        case 400:d=(((pll>>20)&7)+1)*3;branches=B(11)|B(2)|B(1);break;
        case 800:d=((pll>>16)&7)+1;branches=B(10);break;
        case 480:d=((pll>>2)&7)+1;branches=B(8)|B(7);break;default:return 0;}
        uint32_t e=branches<<16;if((g&(branches|e))!=(branches|e)||(s&e)!=e||24*n!=mhz*d)return 0;}
    return ck(p,CCU+0x13c0,0x8700001f,0x81000005)&&ck(p,CCU+0x13c4,B(16),B(16))&&
        ck(p,CCU+0x5c0,B(8),B(8))&&ck(p,CCU+0x5e0,B(28),B(28))&&
        ck(p,CCU+0x1b28,0x30001,0x20000)&&ck(p,0x070101ac,1,1)&&
        ck(p,0x07067000,0x10f,8)&&ck(p,0x07067008,15,8)&&
        ck(p,0x07068000,0x10f,8)&&ck(p,0x07068008,15,8);
}
static int masked(struct a733_usb2_platform *p)
{return ck(p,0x03400000,B(31),0)&&ck(p,0x03400114,B(27),0)&&ck(p,0x03400214,B(27),0)&&ck(p,0x03400314,B(27),0);}
static int globals(struct a733_usb2_platform *p)
{
    uint32_t id,revision,mode,power,phy;
    if(!rd(p,DWC+0xc120,&id)||(id>>16)!=0x3331||!rd(p,DWC+0xc1a0,&revision)||revision!=p->config.revision||
        !rd(p,DWC+0xc140,&mode)||((mode&3)!=1&&(mode&3)!=2)||!rd(p,DWC+0xc144,&power)||
        ((power>>24)&3)>1||!rd(p,DWC+0xc14c,&phy)||(((phy>>2)&3)!=1&&((phy>>2)&3)!=3)||
        (p->config.superspeed&&(!(phy&3)||(phy&3)>2)))return 0;
    /* Board phy_type=utmi: eight-bit interface, source-defined turnaround9.
     * Both suspend quirks and ENBLSLPM are disabled for the active profile. */
    if(!update(p,DWC+0xc200,0x80003d58,9u<<10)||!update(p,DWC+0xc2c0,B(31)|B(17),0))return 0;
    uint32_t ctl=(1500u<<19)|B(12);
    if(revision==0x3230302a&&((power>>24)&3))ctl|=1;
    /* 24MHz reference: integer period41ns, residual adjustment2032, decrement10. */
    return update(p,DWC+0xc110,0xfff8383b,ctl)&&
        update(p,DWC+0xc12c,0xffc00000,41u<<22)&&
        update(p,DWC+0xc630,0xff3fff00,(10u<<24)|(2032u<<8));
}
enum a733_usb2_result a733_usb2_platform_poll(struct a733_usb2_platform *p)
{
    if(!enter(p))return A733_USB2_BUSY;
    if(p->failed)return finish(p,fault(p));
    p->deadline=p->phase==READY?p->config.lifetime_deadline:p->config.deadline;
    if(!timely(p))return finish(p,fault(p));int h=health(p);
    if(h<0)return finish(p,fault(p));if(!h)return finish(p,A733_USB2_PENDING);
    if(p->phase>ATTACH){h=attachment(p);if(h<0)return finish(p,fault(p));if(!h)return finish(p,A733_USB2_PENDING);}
    switch(p->phase){
    case CLOCK:{enum a733_clock_result r=a733_clock_acquire(p->clocks,p->config.owner,
        A733_CLOCK_OSC|A733_CLOCK_REF|A733_CLOCK_PERI0|A733_CLOCK_FABRIC|A733_CLOCK_SERDES_COMMON|A733_CLOCK_USB_PCIE_DOMAIN,
        A733_CLOCK_USB2|(p->config.superspeed?A733_CLOCK_COMB0:0),p->deadline,&p->clock);if(r==A733_CLOCK_BUSY)break;
        if(r!=A733_CLOCK_OK)return finish(p,fault(p));++p->phase;break;}
    case RAIL:{a733_rail_mask_t mask=0;for(unsigned i=0;i<A733_USB2_SUPPLIES+(p->config.superspeed?1u:0u);++i)mask|=A733_RAIL_BIT(supplies[i]);
        enum a733_rail_result r=a733_rail_acquire(p->rails,p->config.owner,mask,0,p->deadline,&p->rail);
        if(r==A733_RAIL_BUSY)break;if(r!=A733_RAIL_PENDING)return finish(p,fault(p));++p->phase;break;}
    case RAIL_WAIT:{enum a733_rail_result r=a733_rail_poll(p->rails,&p->rail);
        if(r==A733_RAIL_BUSY||r==A733_RAIL_PENDING)break;if(r!=A733_RAIL_READY)return finish(p,fault(p));++p->phase;break;}
    case ADMIT:{if(p->ops.admit(p->cookie,&p->config)!=1||!timely(p))return finish(p,fault(p));
        enum a733_pmic_status r=a733_pmic_identity(p->pmic,&p->pmic_lifetime);
        if(r==A733_PMIC_BUSY)break;if(r!=A733_PMIC_READY)return finish(p,fault(p));++p->phase;break;}
    case PCK:{struct a733_gpu_power_resources ready;
        enum a733_gpu_resource_result r=a733_gpu_resources_acquire(p->pck,p->config.pck_client,
            A733_GPU_RESOURCE_OSC|A733_GPU_RESOURCE_PCK,p->deadline,&p->pck_lease.token,&ready);
        if(r==A733_GPU_RESOURCE_BUSY)break;if(r!=A733_GPU_RESOURCE_OK)return finish(p,fault(p));
        p->pck_lease.mask=A733_GPU_RESOURCE_OSC|A733_GPU_RESOURCE_PCK;p->pck_lease.mode=A733_GPU_RESOURCE_STABLE;++p->phase;break;}
    case SUPPLY:{enum a733_pmic_status r=a733_pmic_live(p->pmic,&p->pmic_lifetime);
        if(r==A733_PMIC_BUSY||r==A733_PMIC_PENDING)break;if(r!=A733_PMIC_READY)return finish(p,fault(p));
        if(!p->request.id){r=a733_pmic_submit(p->pmic,p->config.owner,&p->rail,supplies[p->supply_index],p->deadline,&p->request);
            if(r!=A733_PMIC_BUSY&&r!=A733_PMIC_PENDING)return finish(p,fault(p));}
        else{struct a733_pmic_snapshot *s=&p->supplies[p->supply_index];r=a733_pmic_result(p->pmic,&p->request,s);
            if(r==A733_PMIC_BUSY||r==A733_PMIC_PENDING)break;
            if(r!=A733_PMIC_COMPLETE||s->rail!=supplies[p->supply_index]||s->page_before||s->page_after||s->chip!=3||s->revision||
                !s->enabled||!s->decoded||s->programmed_uv!=(p->supply_index==A733_USB2_SUPPLIES?800000u:p->config.supply_uv[p->supply_index]))return finish(p,fault(p));
            r=a733_pmic_reap(p->pmic,&p->request);if(r==A733_PMIC_BUSY)break;
            if(r!=A733_PMIC_COMPLETE)return finish(p,fault(p));p->request=(struct a733_pmic_request){0};
            if(++p->supply_index==A733_USB2_SUPPLIES+(p->config.superspeed?1u:0u))++p->phase;}break;}
    case ATTACH:h=attachment(p);if(h<0)return finish(p,fault(p));if(h==1)++p->phase;break;
    case READY:return finish(p,A733_USB2_READY);
    default:{int l=p->ops.enter(p->cookie,p->deadline);if(!l)break;
        if(l!=1)return finish(p,fault(p));p->held=1;if(!permitted(p))return finish(p,fault(p));
        int ok=parents(p)&&masked(p);
        if(ok)switch(p->phase){
        case BASELINE:ok=ck(p,CCU+0x1348,B(31),0)&&ck(p,CCU+0x1350,B(31),0)&&
            ck(p,CCU+0x1354,B(31),0)&&ck(p,CCU+0x135c,B(16),0)&&
            ck(p,CCU+0x1364,B(31),0)&&ck(p,0x06c00008,0x330010,0);break;
        case CLOCKS:ok=update(p,CCU+0x1348,0x87000000,0x80000000)&&
            update(p,CCU+0x1350,0x8100001f,0x81000000)&&update(p,CCU+0x1354,0x8700001f,0x80000000)&&
            update(p,CCU+0x1364,0x8700001f,p->config.superspeed?0:0x81000003)&&update(p,0x06c00008,0x330010,p->config.superspeed?0x30010:0x130010)&&
            update(p,CCU+0x135c,B(16),B(16));break;
        case PHY_SETUP:ok=ck(p,PHY+0x10,0x107183,0x2000)&&
            update(p,PHY+0x18,UINT32_MAX,0x143338d6)&&update(p,PHY+0x10,0x428,0x420);break;
        case SS_SETUP:
            if(p->config.superspeed){int r=a733_usb3_phy_poll(p);if(r<0)ok=0;else if(!r)return finish(p,A733_USB2_PENDING);}
            break;
        case GLOBAL:ok=globals(p);if(ok&&p->config.superspeed)ok=update(p,DWC+0xd024,B(30),B(30));break;
        default:ok=0;break;}
        if(!ok)return finish(p,fault(p));++p->phase;break;}}
    return finish(p,p->phase==READY?A733_USB2_READY:A733_USB2_PENDING);
}
int a733_usb2_platform_live(struct a733_usb2_platform *p,uint64_t generation,uint64_t deadline)
{
    if(!enter(p))return 0;
    if(p->failed||p->phase!=READY||!generation||generation!=p->attachment.generation||
        !deadline||deadline>p->config.lifetime_deadline){finish(p,fault(p));return -1;}
    p->deadline=deadline;int r=timely(p)?health(p):-1;if(r==1)r=attachment(p);
    if(r<0)fault(p);finish(p,r<0?A733_USB2_RETAINED:A733_USB2_PENDING);return r;
}
void a733_usb2_platform_lost(struct a733_usb2_platform *p)
{if(enter(p))finish(p,fault(p));}
uint32_t a733_usb2_platform_mask(uintptr_t a)
{
    switch(a){case CCU+0x1348:return 0x87000000;case CCU+0x1350:return 0x8100001f;
    case CCU+0x1354:case CCU+0x1364:return 0x8700001f;case CCU+0x135c:return B(16);
    case 0x06c00008:return 0x330010;case PHY+0x10:return 0x428;case PHY+0x18:return UINT32_MAX;
    case DWC+0xc200:return 0x80003d58;case DWC+0xc2c0:return B(31)|B(17);
    case DWC+0xc110:return 0xfff8383b;case DWC+0xc12c:return 0xffc00000;case DWC+0xc630:return 0xff3fff00;
    default:return 0;}
}
int a733_usb2_platform_address(uintptr_t a,unsigned writing)
{
    if(a&3)return 0;if(a733_usb2_platform_mask(a))return 1;if(writing)return 0;
    switch(a){case CCU:case CCU+0xa0:case CCU+0xa8:case CCU+0xac:case CCU+0x500:case CCU+0x580:
    case CCU+0x588:case CCU+0x1908:case CCU+0x1988:case CCU+0x13c0:case CCU+0x13c4:
    case CCU+0x5c0:case CCU+0x5e0:case CCU+0x1b28:case 0x07090160:case 0x070101ac:
    case 0x07067000:case 0x07067008:case 0x07068000:case 0x07068008:
    case 0x03400000:case 0x03400114:case 0x03400214:case 0x03400314:
    case DWC+0xc120:case DWC+0xc1a0:case DWC+0xc140:case DWC+0xc144:case DWC+0xc14c:return 1;
    default:return 0;}
}
