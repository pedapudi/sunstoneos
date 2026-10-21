/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Fixed A733 CCU fields and enable order adapted from pinned ccu-sun60iw2.c
 * and sunxi_ce.c. Copyright(c) 2020 - 2023 Allwinner Technology Co.,Ltd.
 * All rights reserved. See docs/crypto-application.md. */
#include "a733_ce_clock.h"
#include "a733_clock_reference.h"
#define CCU UINT32_C(0x02002000)
#define BIT(n) (UINT32_C(1)<<(n))
#define STABLE (A733_CLOCK_OSC|A733_CLOCK_REF|A733_CLOCK_PERI0|A733_CLOCK_FABRIC)
static int live(struct a733_ce_clock *p)
{
    uint64_t t;
    if(p->self!=p||p->state!=A733_CE_CLOCK_ATTEMPTING||
       p->ops.authorize(p->cookie)!=1||
       a733_clock_token_status(p->registry,&p->token)!=A733_CLOCK_OK||
       p->ops.now_us(p->cookie,&t)!=1||t<p->last_now||t>=p->config.deadline_us)return 0;
    p->last_now=t;return 1;
}
static int read32(struct a733_ce_clock *p,uintptr_t a,uint32_t *v)
{return p->held&&live(p)&&p->ops.read(p->cookie,a,v)==1&&live(p);}
static int bits(struct a733_ce_clock *p,uintptr_t a,uint32_t mask,uint32_t want)
{uint32_t v;return read32(p,a,&v)&&(v&mask)==want;}
static int update(struct a733_ce_clock *p,uintptr_t a,uint32_t mask,uint32_t want)
{
    uint32_t old,got;
    if(!read32(p,a,&old)||!live(p))return 0;
    uint32_t value=(old&~mask)|(want&mask);
    ++p->writes; /* A refused callback may still leave write completion uncertain. */
    return p->ops.write(p->cookie,a,value)==1&&live(p)&&read32(p,a,&got)&&got==value;
}
static int parents(struct a733_ce_clock *p)
{
    uint32_t osc,ref,pll;
    if(!read32(p,UINT32_C(0x07090160),&osc)||!read32(p,CCU,&ref)||
       !a733_clock_ref24_valid(osc,ref)||!read32(p,CCU+0xa0,&pll)||
       !a733_clock_peri0_valid(pll)||
       !bits(p,CCU+0xa8,BIT(31),0)||!bits(p,CCU+0xac,0x8e000000,0))return 0;
    static const unsigned offset[]={0x500,0x580,0x588};
    static const uint32_t masks[]={0x0300001f,0x8700001f,0x8700001f};
    static const unsigned mhz[3][8]={{24,0,0,600,0,0,0,0},
        {24,0,0,600,0,0,0,0},{24,600,0,0,400,0,0,0}};
    uint32_t gates=BIT(11)|BIT(2)|BIT(1);
    for(unsigned i=0;i<3;i++){
        uint32_t v=p->config.fabric[i];unsigned rate=mhz[i][(v>>24)&7],div=(v&31)+1;
        if((v&~masks[i])||(i&&!(v&BIT(31)))||!rate||
           (rate*1000000u)%div||p->config.fabric_hz[i]!=rate*1000000u/div||
           !bits(p,CCU+offset[i],masks[i],v))return 0;
        if(rate==600)gates|=BIT(9);
    }
    return bits(p,CCU+0x1908,gates|(gates<<16),gates|(gates<<16))&&
        bits(p,CCU+0x1988,gates<<16,gates<<16);
}
static int failed(struct a733_ce_clock *p)
{
    p->state=A733_CE_CLOCK_RETAINED;
    if(p->held)p->ops.retain(p->cookie); /* Never unlock uncertain local writes. */
    (void)a733_clock_retain(p->registry,&p->token);
    return 0;
}
int a733_ce_clock_provision(struct a733_ce_clock *p,const struct a733_ce_config *c,
    const struct a733_ce_clock_ops *o,void *cookie,struct a733_clock_leases *r,
    const struct a733_clock_token *t)
{
    if(!p||p->state!=A733_CE_CLOCK_NEW||!c||!o||!r||!t||!cookie||
       !c->owner||!c->handoff||!c->deadline_us||c->profile!=A733_CE_SHA256_AES128||
       c->health_profile||c->configure_health||!t->id||t->owner!=c->owner||
       t->exclusive!=A733_CLOCK_CE||t->shared!=STABLE||
       !o->authorize||!o->now_us||!o->enter||!o->leave||!o->retain||!o->read||!o->write)return 0;
    const void *objects[]={p,c,o,r,t};
    const size_t sizes[]={sizeof(*p),sizeof(*c),sizeof(*o),sizeof(*r),sizeof(*t)};
    for(unsigned i=0;i<5;i++)for(unsigned j=i+1;j<5;j++)
        if(!a733_ce_separate(objects[i],sizes[i],objects[j],sizes[j]))return 0;
    for(unsigned i=0;i<4;i++)if(c->health[i])return 0;
    *p=(struct a733_ce_clock){.self=p,.config=*c,.ops=*o,.registry=r,.token=*t,
        .cookie=cookie,.state=A733_CE_CLOCK_ATTEMPTING};
    if(!live(p)||p->ops.enter(cookie,c->deadline_us)!=1)return failed(p);
    p->held=1;
    /* MBUS40302 is real neighboring gate state, not a write-only key. */
    if(!parents(p)||!bits(p,CCU+0xac0,0x8700001f,0)||
       !bits(p,CCU+0xac4,0x30003,0)||!bits(p,CCU+0x5e4,0x40306,0x40302))return failed(p);
    if(!update(p,CCU+0xac0,0x8700001f,0x01000000)||
       !update(p,CCU+0xac4,0x30000,0x30000)||
       !update(p,CCU+0xac4,1,1)||
       !update(p,CCU+0xac0,BIT(31),BIT(31))||
       !update(p,CCU+0x5e4,4,4)||!update(p,CCU+0xac4,2,2)||
       !parents(p)||!bits(p,CCU+0xac0,0x8700001f,0x81000000)||
       !bits(p,CCU+0xac4,0x30003,0x30003)||!bits(p,CCU+0x5e4,0x40306,0x40306)||!live(p))return failed(p);
    p->ops.leave(cookie);p->held=0;
    if(!live(p))return failed(p);
    p->state=A733_CE_CLOCK_READY;return 1;
}
